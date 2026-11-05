from __future__ import annotations

import re

import httpx


class ModelError(RuntimeError):
    pass


class ModelClient:
    def __init__(self, config: dict, transport=None):
        self.config = config
        self.client = httpx.AsyncClient(timeout=config.get("timeout_seconds", 90), transport=transport,
                                       follow_redirects=False, trust_env=True)
        self.calls = 0
        self.tokens = 0
        self.tool_reasoning_none = False

    async def complete(self, messages: list[dict], tools: list[dict] | None = None) -> dict:
        c = self.config
        if not c.get("api_key") or not c.get("base_url") or not c.get("model"):
            raise ModelError("模型尚未配置。请在 config.yaml 中填写接口、模型和密钥。")
        endpoint = c["base_url"].rstrip("/")
        if not endpoint.endswith("/chat/completions"):
            endpoint += "/chat/completions"
        # Keep the default request minimal: reasoning providers often reject
        # temperature or max_tokens. Their own output defaults apply.
        body = {"model": c["model"], "messages": messages, "stream": False}
        for option in ("temperature", "max_tokens", "max_completion_tokens", "reasoning_effort"):
            if c.get(option) is not None:
                body[option] = c[option]
        if tools:
            body.update(tools=tools, tool_choice="auto")
        for key, value in (c.get("extra_body") or {}).items():
            if key in {"messages", "model", "tools", "tool_choice", "stream"}:
                raise ModelError("extra_body 不能覆盖模型、消息或工具协议。")
            body[key] = value
        if tools and self.tool_reasoning_none:
            body["reasoning_effort"] = "none"
        self.calls += 1
        try:
            response = await self.client.post(endpoint, headers={"Authorization": "Bearer " + c["api_key"]}, json=body)
        except httpx.TimeoutException:
            raise ModelError("模型响应超时。本次没有自动重试，以避免重复消耗额度。") from None
        except httpx.HTTPError:
            raise ModelError("无法连接模型接口，请检查地址、网络或代理配置。") from None
        # Retry only an explicit protocol rejection with the exact remedy;
        # never retry auth/quota failures or silently change the model.
        if response.status_code == 400 and tools and body.get("reasoning_effort") != "none":
            try:
                error = response.json().get("error", {})
                detail = error.get("message", "") if isinstance(error, dict) else ""
            except (ValueError, AttributeError):
                detail = ""
            if "Function tools with reasoning_effort are not supported" in detail and "'none'" in detail:
                self.tool_reasoning_none = True
                body["reasoning_effort"] = "none"
                self.calls += 1
                try:
                    response = await self.client.post(endpoint, headers={"Authorization": "Bearer " + c["api_key"]}, json=body)
                except httpx.HTTPError:
                    raise ModelError("工具兼容请求连接失败，请稍后重试。") from None
        if response.status_code >= 400:
            try:
                code = str(response.json().get("error", {}).get("code", ""))
            except (ValueError, AttributeError, TypeError):
                code = ""
            if response.status_code == 429 and code == "1305":
                raise ModelError("当前模型访问量过大（1305），请稍后重试，或在本地配置中切换可用模型。本次未自动重试。")
            hints = {401: "密钥无效或已失效", 403: "接口权限不足", 404: "接口地址或模型名称不正确", 429: "调用过频或额度不足"}
            detail = ""
            if response.status_code == 400:
                try:
                    error = response.json().get("error", {})
                    detail = str(error.get("message", "")) if isinstance(error, dict) else ""
                    detail = detail.replace(c["api_key"], "<redacted>")
                    detail = re.sub(r"sk-[A-Za-z0-9_-]+", "<redacted>", detail)[:700]
                except (ValueError, AttributeError):
                    pass
            raise ModelError(f"模型接口返回 HTTP {response.status_code}（{hints.get(response.status_code, '请求参数不兼容' if response.status_code == 400 else '服务暂不可用')}）。" + (" 接口说明：" + detail if detail else ""))
        try:
            payload = response.json()
            self.tokens += int(payload.get("usage", {}).get("total_tokens", 0))
            choice = payload["choices"][0]
            message = choice["message"]
            if choice.get("finish_reason") == "length":
                raise ModelError("模型输出达到长度上限，请缩小问题范围，或配置提供商支持的输出预算参数。")
            if not message.get("content") and not message.get("tool_calls"):
                raise ModelError("模型没有返回文本或工具调用。请检查模型参数。")
            return message
        except (KeyError, IndexError, TypeError, ValueError):
            raise ModelError("模型响应格式与 Chat Completions 协议不兼容。") from None

    async def close(self) -> None:
        await self.client.aclose()
