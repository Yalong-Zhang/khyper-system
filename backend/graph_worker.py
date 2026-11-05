from __future__ import annotations

import asyncio
import json
import os
import subprocess
from collections import deque

from backend.config import project_path
from backend.tools import validate_arguments


def js_safe(value):
    if type(value) is int and abs(value)>2**53-1:return str(value)
    if isinstance(value,dict):return {k:js_safe(v) for k,v in value.items()}
    if isinstance(value,list):return [js_safe(v) for v in value]
    return value


class GraphWorker:
    def __init__(self, config: dict):
        self.config = config["graph"]
        self.process: asyncio.subprocess.Process | None = None
        self.lock = asyncio.Lock()
        self.info: dict = {}
        self.state = "empty"
        self.error = ""
        self.sequence = 0
        self.progress = {}
        self.stderr: deque[str] = deque(maxlen=4)
        self.stderr_task: asyncio.Task | None = None

    @property
    def ready(self) -> bool:
        return bool(self.process and self.process.returncode is None and self.state == "ready")

    async def _drain_stderr(self) -> None:
        assert self.process and self.process.stderr
        async for line in self.process.stderr:
            self.stderr.append(line.decode("utf-8", errors="replace").strip()[:500])

    async def start(self) -> None:
        # Dataset selection is explicit; never load a domain-specific default.
        return

    async def load(self, dataset: dict) -> None:
        await self.stop()
        self.state, self.error, self.info = "loading", "", {}
        self.progress = {"phase": "starting", "records": 0}
        try:
            executable = project_path(self.config["worker"])
            if not executable.exists():
                raise RuntimeError("图工具尚未构建，请运行 setup.ps1。")
            self.process = await asyncio.create_subprocess_exec(
                str(executable), stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                limit=4 * 1024 * 1024,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
            )
            self.stderr.clear()
            self.stderr_task = asyncio.create_task(self._drain_stderr())
            # Send only a path. C++ reads and validates both source tables itself.
            request = json.dumps({"descriptor_path": str(dataset["_descriptor"])}, ensure_ascii=False)
            self.process.stdin.write((request + "\n").encode("utf-8"))
            await self.process.stdin.drain()
            async with asyncio.timeout(self.config.get("startup_timeout_seconds", 600)):
                while True:
                    line = await self.process.stdout.readline()
                    if not line:
                        await self.process.wait()
                        await self.stderr_task
                        detail = " ".join(self.stderr) or f"工作进程退出码 0x{self.process.returncode & 0xffffffff:08x}，请运行 setup.ps1 检查原生依赖。"
                        raise RuntimeError("图加载失败：" + detail)
                    message = json.loads(line)
                    if message.get("type") == "progress":
                        self.progress = {k: message[k] for k in ("phase", "records")}
                    elif message.get("type") == "ready":
                        break
                    else:
                        raise RuntimeError("图工具返回未知加载消息。")
            self.info = js_safe(message["data"])
            self.info["descriptor_path"] = str(dataset["_descriptor"])
            self.state = "ready"
        except BaseException as exc:
            await asyncio.sleep(0)
            detail = " ".join(self.stderr)
            await self.stop()
            self.info = {}
            self.state = "error"
            self.error = (str(exc) + (" " + detail if detail else ""))[:700] or "加载失败或已中止。"
            raise

    async def call(self, tool: str, arguments: dict) -> dict:
        async with self.lock:
            if not self.ready:
                raise RuntimeError("图工具未就绪，请重启服务。")
            validate_arguments(tool, arguments, self.info.get("directed", True))
            self.sequence += 1
            request_id = self.sequence
            payload = json.dumps({"id": request_id, "tool": tool, "arguments": arguments}, ensure_ascii=False)
            assert self.process and self.process.stdin and self.process.stdout
            try:
                self.process.stdin.write((payload + "\n").encode("utf-8"))
                await self.process.stdin.drain()
                line = await asyncio.wait_for(self.process.stdout.readline(), self.config["tool_timeout_seconds"])
                if not line:
                    raise RuntimeError("图工具意外退出。")
                response = json.loads(line)
                if response.get("id") != request_id:
                    raise RuntimeError("图工具响应序号不匹配。")
            except BaseException:
                await self.stop()
                self.state, self.error = "error", "图工具连接已中断，请重启服务。"
                raise
            if not response.get("ok"):
                raise ValueError(response.get("error", "图查询失败。"))
            return js_safe(response["data"])

    async def stop(self) -> None:
        process, self.process = self.process, None
        if process and process.returncode is None:
            process.terminate()
            try:
                await asyncio.wait_for(process.wait(), 5)
            except asyncio.TimeoutError:
                process.kill()
                await process.wait()
        if self.stderr_task:
            self.stderr_task.cancel()
            await asyncio.gather(self.stderr_task, return_exceptions=True)
            self.stderr_task = None
        if self.state == "ready":
            self.state = "stopped"
