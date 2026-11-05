from __future__ import annotations

import asyncio
import json
import sys
import hashlib
import os
import subprocess
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field, field_validator

from backend.agent import analyze
from backend.datasets import descriptor
from backend.config import ROOT, load_config
from backend.graph_worker import GraphWorker
from backend.llm import ModelError


class Question(BaseModel):
    dataset_id: str | None = None
    question: str = Field(min_length=1, max_length=2000)

    @field_validator("question")
    @classmethod
    def nonblank(cls, value: str) -> str:
        if not value.strip():
            raise ValueError("请输入分析问题。")
        return value.strip()


class DatasetPath(BaseModel):
    path: str = Field(min_length=1, max_length=4096)


def create_app(config: dict | None = None, graph=None) -> FastAPI:
    config = config or load_config()
    worker = graph or GraphWorker(config)
    busy = asyncio.Lock()
    load_task = None
    picker_busy = asyncio.Lock()

    @asynccontextmanager
    async def lifespan(app):
        async def initialize():
            try:
                await worker.start()
            except Exception:
                pass  # Health endpoint exposes a bounded, local startup error.
        startup = asyncio.create_task(initialize())
        yield
        if load_task:
            load_task.cancel()
            await asyncio.gather(load_task, return_exceptions=True)
        startup.cancel()
        await asyncio.gather(startup, return_exceptions=True)
        await worker.stop()

    app = FastAPI(title="KHyper 通用图分析", lifespan=lifespan)
    app.state.worker = worker
    app.mount("/assets", StaticFiles(directory=ROOT / "frontend"), name="assets")

    @app.middleware("http")
    async def fresh_assets(request, call_next):
        response = await call_next(request)
        if request.url.path == "/" or request.url.path.startswith("/assets/"):
            response.headers["Cache-Control"] = "no-store"
        return response

    @app.get("/")
    async def index():
        html = (ROOT / "frontend/index.html").read_text(encoding="utf-8")
        for name in ("styles.css", "app.js"):
            digest = hashlib.sha256((ROOT / "frontend" / name).read_bytes()).hexdigest()[:12]
            html = html.replace(f"/assets/{name}", f"/assets/{name}?v={digest}")
        return HTMLResponse(html, headers={"Cache-Control": "no-store"})

    @app.get("/api/health")
    async def health():
        llm = config["llm"]
        return {"status": "ready" if worker.ready else worker.state, "busy": busy.locked(),
                "dataset": worker.info, "error": worker.error,
                "model": llm.get("model", ""),
                "model_configured": bool(llm.get("model") and llm.get("api_key") and llm.get("base_url")),
                "progress": getattr(worker, "progress", {}),
                "limits": {"tools": config["agent"]["max_tool_calls"], "model_calls": config["agent"]["max_model_calls"]}}

    @app.post("/api/datasets/pick")
    async def pick():
        if picker_busy.locked():
            raise HTTPException(409, "文件选择窗口已打开。")
        async with picker_busy:
            process = await asyncio.create_subprocess_exec(sys.executable, str(ROOT / "backend/pick_dataset.py"), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            try:
                output, _ = await asyncio.wait_for(process.communicate(), 180)
            except asyncio.TimeoutError:
                if process.returncode is None:
                    process.kill()
                await process.wait()
                raise HTTPException(408, "文件选择已超时，请重新打开或直接输入路径。") from None
            except BaseException:
                if process.returncode is None:
                    process.kill()
                await process.wait()
                raise
            if process.returncode:
                raise HTTPException(503, "无法打开本机选择窗口，请直接输入描述文件的绝对路径。")
            return {"path": output.decode("utf-8").strip()}

    @app.get("/api/datasets/examples")
    async def examples():
        return [{"name": p.stem, "path": str(p.resolve())} for p in sorted((ROOT/"examples/datasets").glob("*.json"))]

    @app.post("/api/datasets/load")
    async def select(body: DatasetPath):
        nonlocal load_task
        if busy.locked():
            raise HTTPException(409, "请先等待当前加载或分析完成。")
        try:
            selected = await asyncio.to_thread(descriptor, body.path.strip().strip('"'))
        except (ValueError, OSError, KeyError) as exc:
            raise HTTPException(400, str(exc)[:700]) from None
        if busy.locked():
            raise HTTPException(409, "已有任务正在执行。")
        await busy.acquire()
        worker.state = "loading"
        async def load():
            try:
                await worker.load(selected)
            except Exception:
                pass  # Bounded loading error is exposed by health.
            finally:
                busy.release()
        load_task = asyncio.create_task(load())
        return {"status": "loading", "name": selected["name"]}

    @app.post("/api/analyze")
    async def run(body: Question, request: Request):
        if body.dataset_id and body.dataset_id != worker.info.get("id"):
            raise HTTPException(409, "数据集已切换，请刷新页面后重新分析。")
        if not worker.ready:
            raise HTTPException(503, worker.error or "图数据尚在加载，请稍候。")
        if not all(config["llm"].get(k) for k in ("model", "api_key", "base_url")):
            raise HTTPException(503, "请先配置模型接口和 API key。")
        if busy.locked():
            raise HTTPException(409, "已有分析任务正在执行，请等待结束。")
        await busy.acquire()

        async def stream():
            queue: asyncio.Queue = asyncio.Queue(maxsize=64)

            async def produce():
                try:
                    async with asyncio.timeout(config["agent"]["task_timeout_seconds"]):
                        async for event in analyze(body.question, config, worker):
                            await queue.put(event)
                except asyncio.TimeoutError:
                    await queue.put({"type": "error", "message": "分析达到时间上限，已停止本次任务。"})
                except (ModelError, RuntimeError, ValueError) as exc:
                    await queue.put({"type": "error", "message": str(exc)[:500]})
                except Exception:
                    await queue.put({"type": "error", "message": "分析遇到内部错误，请检查本地服务或重新提交。"})
                finally:
                    # A disconnected consumer must not leave a cancelled
                    # producer blocked trying to append to its full queue.
                    if not asyncio.current_task().cancelling():
                        await queue.put(None)

            producer = asyncio.create_task(produce())
            try:
                yield json.dumps({"type": "start", "question": body.question}, ensure_ascii=False) + "\n"
                while True:
                    if await request.is_disconnected():
                        break
                    try:
                        event = await asyncio.wait_for(queue.get(), 5)
                    except asyncio.TimeoutError:
                        yield '{"type":"heartbeat"}\n'
                        continue
                    if event is None:
                        break
                    yield json.dumps(event, ensure_ascii=False) + "\n"
            finally:
                producer.cancel()
                await asyncio.gather(producer, return_exceptions=True)
                busy.release()

        return StreamingResponse(stream(), media_type="application/x-ndjson", headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"})

    return app


app = create_app()
