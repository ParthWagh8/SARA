import asyncio
import json
import time
from contextlib import asynccontextmanager, suppress
from pathlib import Path
from typing import Set

import cv2
import numpy as np
import websockets
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from ultralytics import YOLO

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
CAPTURE_DIR = BASE_DIR / "captures"
CAPTURE_DIR.mkdir(exist_ok=True)

# ESP32 soft-AP defaults. Change these if your network configuration differs.
ESP32_HOST = "192.168.4.1"
CAMERA_WS_URL = f"ws://{ESP32_HOST}/Camera"
CONTROL_WS_URL = f"ws://{ESP32_HOST}/CarInput"

# "yolov8n.pt" downloads the small pretrained model on first launch.
# For offline use, place the weights file here and set MODEL_PATH to its filename.
MODEL_PATH = "yolov8n.pt"
CONFIDENCE_THRESHOLD = 0.35
JPEG_QUALITY = 75

latest_annotated_jpeg: bytes | None = None
latest_raw_jpeg: bytes | None = None
latest_meta: dict = {
    "connected": False,
    "model": MODEL_PATH,
    "people": 0,
    "detections": [],
    "inference_ms": None,
    "fps": 0.0,
    "last_frame_time": None,
    "error": None,
}
video_clients: Set[WebSocket] = set()
video_clients_lock = asyncio.Lock()
capture_lock = asyncio.Lock()
esp32_control_socket = None
model: YOLO | None = None
camera_task: asyncio.Task | None = None
camera_retry_delay = 2


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())


def process_frame(frame: np.ndarray):
    """Run YOLO and return annotated frame plus compact detection metadata."""
    global model
    start = time.perf_counter()
    results = model.predict(
        source=frame,
        conf=CONFIDENCE_THRESHOLD,
        verbose=False,
        imgsz=320,
    )
    result = results[0]
    annotated = frame.copy()
    detections = []
    names = result.names

    if result.boxes is not None:
        for box in result.boxes:
            cls_id = int(box.cls[0].item())
            confidence = float(box.conf[0].item())
            x1, y1, x2, y2 = [int(v) for v in box.xyxy[0].tolist()]
            label = str(names.get(cls_id, cls_id))
            # Person detections are magenta, other classes are cyan.
            color = (255, 0, 255) if label == "person" else (255, 220, 0)
            cv2.rectangle(annotated, (x1, y1), (x2, y2), color, 2)
            text = f"{label} {confidence:.2f}"
            (tw, th), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.55, 2)
            top = max(0, y1 - th - 8)
            cv2.rectangle(annotated, (x1, top), (x1 + tw + 8, top + th + 8), color, -1)
            cv2.putText(
                annotated, text, (x1 + 4, top + th + 2),
                cv2.FONT_HERSHEY_SIMPLEX, 0.55, (15, 15, 15), 2, cv2.LINE_AA
            )
            detections.append({
                "class": label,
                "confidence": round(confidence, 4),
                "box": [x1, y1, x2, y2],
            })

    elapsed_ms = (time.perf_counter() - start) * 1000
    return annotated, detections, elapsed_ms


async def broadcast_video(jpeg: bytes):
    async with video_clients_lock:
        clients = list(video_clients)
    stale = []
    for client in clients:
        try:
            await client.send_bytes(jpeg)
        except Exception:
            stale.append(client)
    if stale:
        async with video_clients_lock:
            for client in stale:
                video_clients.discard(client)


async def camera_receiver():
    """Own the ESP32 camera WebSocket; browser clients connect to this laptop instead."""
    global latest_annotated_jpeg, latest_raw_jpeg, latest_meta, camera_retry_delay
    while True:
        try:
            latest_meta["error"] = None
            async with websockets.connect(
                CAMERA_WS_URL,
                max_size=4 * 1024 * 1024,
                ping_interval=20,
                ping_timeout=20,
                open_timeout=8,
            ) as ws:
                camera_retry_delay = 2
                latest_meta["connected"] = True
                print(f"[camera] Connected to {CAMERA_WS_URL}")
                previous = time.perf_counter()
                fps_smooth = 0.0

                async for message in ws:
                    if not isinstance(message, bytes):
                        continue
                    arr = np.frombuffer(message, dtype=np.uint8)
                    frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                    if frame is None:
                        continue

                    latest_raw_jpeg = message
                    try:
                        annotated, detections, inference_ms = await asyncio.to_thread(
                            process_frame, frame
                        )
                        ok, encoded = cv2.imencode(
                            ".jpg", annotated,
                            [int(cv2.IMWRITE_JPEG_QUALITY), JPEG_QUALITY]
                        )
                        if not ok:
                            continue
                        annotated_jpeg = encoded.tobytes()
                        latest_annotated_jpeg = annotated_jpeg

                        now = time.perf_counter()
                        instantaneous = 1.0 / max(now - previous, 1e-6)
                        previous = now
                        fps_smooth = instantaneous if fps_smooth == 0 else (
                            0.2 * instantaneous + 0.8 * fps_smooth
                        )
                        people = sum(1 for d in detections if d["class"] == "person")
                        latest_meta = {
                            "connected": True,
                            "model": MODEL_PATH,
                            "people": people,
                            "detections": detections,
                            "inference_ms": round(inference_ms, 1),
                            "fps": round(fps_smooth, 1),
                            "last_frame_time": now_iso(),
                            "error": None,
                        }
                        await broadcast_video(annotated_jpeg)
                    except Exception as exc:
                        latest_meta["error"] = f"Inference error: {exc}"
                        print(f"[vision] {exc}")
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            latest_meta["connected"] = False
            latest_meta["error"] = f"Camera connection: {exc}"
            print(f"[camera] {exc}; retrying in {camera_retry_delay}s")
            await asyncio.sleep(camera_retry_delay)
            camera_retry_delay = min(camera_retry_delay * 2, 10)
        finally:
            latest_meta["connected"] = False


@asynccontextmanager
async def lifespan(app: FastAPI):
    global model, camera_task
    print(f"[vision] Loading YOLO model: {MODEL_PATH}")
    model = YOLO(MODEL_PATH)
    # Warm-up once so the first live frame is less delayed.
    try:
        await asyncio.to_thread(model.predict, np.zeros((240, 320, 3), dtype=np.uint8), verbose=False)
    except Exception as exc:
        print(f"[vision] Warm-up warning: {exc}")
    camera_task = asyncio.create_task(camera_receiver())
    yield
    if camera_task:
        camera_task.cancel()
        with suppress(asyncio.CancelledError):
            await camera_task
    if esp32_control_socket is not None:
        with suppress(Exception):
            await esp32_control_socket.send("MoveCar,0")
            await esp32_control_socket.close()


app = FastAPI(title="RoverAI", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/api/status")
async def status():
    return JSONResponse(latest_meta)


@app.post("/api/capture")
async def capture():
    async with capture_lock:
        if latest_annotated_jpeg is None or latest_raw_jpeg is None:
            return JSONResponse({"error": "No processed frame available yet."}, status_code=503)
        stamp = time.strftime("%Y%m%d_%H%M%S", time.localtime())
        annotated_path = CAPTURE_DIR / f"{stamp}_annotated.jpg"
        raw_path = CAPTURE_DIR / f"{stamp}_original.jpg"
        meta_path = CAPTURE_DIR / f"{stamp}.json"
        annotated_path.write_bytes(latest_annotated_jpeg)
        raw_path.write_bytes(latest_raw_jpeg)
        record = {
            "captured_at": now_iso(),
            "annotated_image": str(annotated_path.relative_to(BASE_DIR)),
            "original_image": str(raw_path.relative_to(BASE_DIR)),
            **latest_meta,
        }
        meta_path.write_text(json.dumps(record, indent=2), encoding="utf-8")
        return {"ok": True, "captured_at": record["captured_at"], "metadata": str(meta_path.name)}


@app.websocket("/ws/video")
async def video_socket(websocket: WebSocket):
    await websocket.accept()
    async with video_clients_lock:
        video_clients.add(websocket)
    try:
        # Send latest frame immediately if available.
        if latest_annotated_jpeg:
            await websocket.send_bytes(latest_annotated_jpeg)
        while True:
            # Browser sends no video commands; keep the socket alive.
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    except Exception:
        pass
    finally:
        async with video_clients_lock:
            video_clients.discard(websocket)


@app.websocket("/ws/control")
async def control_socket(websocket: WebSocket):
    global esp32_control_socket
    await websocket.accept()
    try:
        async with websockets.connect(
            CONTROL_WS_URL, max_size=64 * 1024, ping_interval=20, open_timeout=8
        ) as esp_ws:
            esp32_control_socket = esp_ws
            print("[control] Browser control bridge connected to ESP32")
            while True:
                message = await websocket.receive_text()
                # Firmware expects "Key,Value", e.g. "MoveCar,1" or "Speed,150".
                if len(message) <= 80 and "," in message:
                    await esp_ws.send(message)
    except WebSocketDisconnect:
        pass
    except Exception as exc:
        print(f"[control] {exc}")
    finally:
        if esp32_control_socket is not None:
            with suppress(Exception):
                await esp32_control_socket.send("MoveCar,0")
            esp32_control_socket = None
