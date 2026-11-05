from __future__ import annotations

import json
from collections.abc import AsyncIterator

from backend.config import ROOT
from backend.llm import ModelClient, ModelError
from backend.tools import TOOLS, describe_tool, validate_arguments, tools_for


async def analyze(question: str, config: dict, worker, client: ModelClient | None = None) -> AsyncIterator[dict]:
    client = client or ModelClient(config["llm"])
    skill = (ROOT / "backend/skills/graph_analysis.md").read_text(encoding="utf-8")
    skill += "\n\n" + (ROOT / "backend/skills/tool_reference.md").read_text(encoding="utf-8")
    messages = [{"role": "system", "content": skill + "\n\n当前真实数据概况：" + json.dumps({k:v for k,v in worker.info.items() if k != "descriptor_path"}, ensure_ascii=False)},
                {"role": "user", "content": question}]
    tool_count = 0
    successful = 0
    seen: dict[str, dict] = {}
    max_calls = config["agent"]["max_tool_calls"]
    rounds = config["agent"]["max_model_calls"]
    try:
        for turn in range(1, rounds + 1):
            step = f"model-{turn}"
            final_round = turn == rounds or tool_count >= max_calls
            label = "整理已有数据并生成分析报告" if final_round else ("理解问题，选择合适的统计指标" if turn == 1 else "解读统计结果，确定下一步分析")
            yield {"type": "step_start", "id": step, "label": label, "kind": "model"}
            if final_round:
                messages.append({"role": "system", "content": "本轮为最后一轮，不再调用工具。请根据已有结果输出简洁中文报告；缺少数据要明确说明。"})
            message = await client.complete(messages, None if final_round else tools_for(worker.info.get("directed", True)))
            yield {"type": "step_done", "id": step}
            calls = message.get("tool_calls") or []
            if not calls:
                report = (message.get("content") or "").strip()
                if not report:
                    raise ModelError("模型未生成分析报告。")
                # Statistical answers cannot be completed without evidence. A
                # scope/ambiguity explanation can be returned without inventing results.
                yield {"type": "report", "markdown": report, "tool_calls": tool_count,
                       "model_calls": client.calls, "total_tokens": client.tokens, "has_evidence": successful > 0}
                return
            if final_round:
                raise ModelError("模型未遵循最后一轮报告指令。")
            assistant = {"role": "assistant", "content": message.get("content") or "", "tool_calls": calls}
            if message.get("reasoning_content"):
                assistant["reasoning_content"] = message["reasoning_content"]
            messages.append(assistant)
            for call in calls:
                fn = call.get("function", {})
                name = fn.get("name", "")
                step_id = f"tool-{tool_count + 1}"
                if tool_count >= max_calls:
                    result = {"error": "工具调用预算已用尽，请用已有数据完成报告。"}
                else:
                    tool_count += 1
                    try:
                        args = fn.get("arguments", "{}")
                        args = json.loads(args) if isinstance(args, str) else args
                        validate_arguments(name, args, worker.info.get("directed", True))
                        label = describe_tool(name, args)
                        yield {"type": "step_start", "id": step_id, "label": label, "kind": "tool"}
                        key = name + json.dumps(args, sort_keys=True, ensure_ascii=False)
                        cached = key in seen
                        result = seen[key] if cached else await worker.call(name, args)
                        seen[key] = result
                        successful += 1
                        yield {"type": "step_done", "id": step_id, "summary": "已取得真实统计结果" + ("（本任务内复用）" if cached else "")}
                        yield {"type": "evidence", "id": step_id, "label": label, "tool": name, "data": result}
                    except (ValueError, KeyError, TypeError) as exc:
                        result = {"error": str(exc)[:300]}
                        yield {"type": "step_error", "id": step_id, "label": "工具参数需要调整", "message": str(exc)[:300]}
                messages.append({"role": "tool", "tool_call_id": call.get("id", ""), "content": json.dumps(result, ensure_ascii=False)})
        raise ModelError("分析达到模型调用上限，但尚未生成报告。")
    finally:
        await client.close()
