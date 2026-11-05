import asyncio
import json

import httpx
import pytest

from backend.agent import analyze
from backend.config import load_config
from backend.llm import ModelClient, ModelError
from backend.tools import validate_arguments


def test_api_file_parser_and_strict_parameters(tmp_path):
    import shutil
    from backend.config import ROOT
    shutil.copy(ROOT/'config.yaml',tmp_path/'config.yaml')
    import yaml
    config=yaml.safe_load((tmp_path/'config.yaml').read_text(encoding='utf-8'))
    config['llm'].update(api_key='fixture-secret',url='https://example.test/v1',model_name='reasoning-fixture')
    (tmp_path/'config.yaml').write_text(yaml.safe_dump(config),encoding='utf-8')
    (tmp_path/'api.txt').write_text('ignored-obsolete-credential',encoding='utf-8')
    result=load_config(tmp_path)['llm']
    assert result['api_key']=='fixture-secret'
    assert result['base_url']=='https://example.test/v1'
    assert result['model']=='reasoning-fixture'
    with pytest.raises(ValueError):validate_arguments("rank_vertices_weighted",{"metric":"in_degree"})
    with pytest.raises(ValueError):validate_arguments("get_vertex_stats_weighted",{"vertex_id":True})
    with pytest.raises(ValueError):validate_arguments("search_vertices",{"query":"x","shell":"anything"})


class FakeGraph:
    info={"vertices":4,"edges":4}
    def __init__(self):self.calls=[]
    async def call(self,name,args):
        self.calls.append((name,args))
        return {"rows":[{"vertex_id":"10","name":"Alpha","value":14}],"metric":"total"}


def fixture_config():
    c=load_config()
    c["llm"].update(api_key="fixture-secret",base_url="https://example.test/v1",model="fixture")
    return c


def test_agent_executes_tools_and_uses_fresh_context():
    async def run():
        for _ in range(2):
            requests=[]
            def handler(request):
                body=json.loads(request.content);requests.append(body)
                assert not {"thinking","temperature","max_tokens"}.intersection(body)
                if len(requests)==1:
                    assert len(body["messages"])==2
                    message={"role":"assistant","content":None,"tool_calls":[{"id":"one","type":"function","function":{"name":"rank_vertices_weighted","arguments":'{"metric":"total","limit":1}'}}]}
                else:
                    assert body["messages"][-1]["role"]=="tool"
                    assert 'Alpha' in body["messages"][-1]["content"]
                    message={"role":"assistant","content":"# 活跃公司\n\nAlpha 的总流动权重为 14。"}
                return httpx.Response(200,json={"choices":[{"message":message,"finish_reason":"stop"}],"usage":{"total_tokens":20}})
            c=fixture_config();graph=FakeGraph()
            events=[e async for e in analyze("最活跃的公司",c,graph,ModelClient(c["llm"],httpx.MockTransport(handler)))]
            assert graph.calls==[("rank_vertices_weighted",{"metric":"total","limit":1})]
            assert events[-1]["type"]=="report" and events[-1]["has_evidence"]
            assert events[-1]["model_calls"]==2
    asyncio.run(run())


def test_model_error_never_echoes_credentials():
    async def run():
        c=fixture_config()
        def handler(request):return httpx.Response(401,json={"error":"fixture-secret echoed upstream"})
        client=ModelClient(c["llm"],httpx.MockTransport(handler))
        try:
            with pytest.raises(ModelError) as e:await client.complete([{"role":"user","content":"x"}])
            assert 'fixture-secret' not in str(e.value)
            assert '401' in str(e.value)
            assert client.calls==1
        finally:await client.close()
    asyncio.run(run())


def test_budget_stops_additional_execution():
    async def run():
        requests=[]
        def handler(request):
            body=json.loads(request.content);requests.append(body)
            if len(requests)==1:
                calls=[{"id":str(i),"type":"function","function":{"name":"rank_vertices_weighted","arguments":'{"metric":"total"}'}} for i in range(3)]
                message={"role":"assistant","content":"","tool_calls":calls}
            else:
                assert "tools" not in body
                message={"role":"assistant","content":"已达到预算。Alpha 为已有结果中的第一名。"}
            return httpx.Response(200,json={"choices":[{"message":message,"finish_reason":"stop"}]})
        c=fixture_config();c["agent"]["max_tool_calls"]=1;graph=FakeGraph()
        events=[e async for e in analyze("排名",c,graph,ModelClient(c["llm"],httpx.MockTransport(handler)))]
        assert len(graph.calls)==1 and events[-1]["tool_calls"]==1
        assert len(requests)==2
    asyncio.run(run())


def test_model_busy_error_is_distinct_from_account_quota():
    async def run():
        c=fixture_config()
        client=ModelClient(c['llm'],httpx.MockTransport(lambda request:httpx.Response(429,json={'error':{'code':'1305','message':'busy'}})))
        try:
            with pytest.raises(ModelError,match='1305') as e:await client.complete([{'role':'user','content':'x'}])
            assert '额度不足' not in str(e.value)
        finally:await client.close()
    asyncio.run(run())


def test_explicit_tool_reasoning_compatibility_retry():
    async def run():
        bodies=[]
        def handler(request):
            body=json.loads(request.content);bodies.append(body)
            if len(bodies)==1:
                return httpx.Response(400,json={'error':{'message':"Function tools with reasoning_effort are not supported. Set reasoning_effort to 'none'."}})
            assert body['reasoning_effort']=='none'
            return httpx.Response(200,json={'choices':[{'message':{'role':'assistant','content':'OK'},'finish_reason':'stop'}]})
        c=fixture_config();client=ModelClient(c['llm'],httpx.MockTransport(handler))
        from backend.tools import TOOLS
        try:
            await client.complete([{'role':'user','content':'x'}],TOOLS)
            await client.complete([{'role':'user','content':'x'}],TOOLS)
            assert len(bodies)==3 and client.tool_reasoning_none
        finally:await client.close()
    asyncio.run(run())
