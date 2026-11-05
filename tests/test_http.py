import asyncio
import json

import httpx

from backend.app import create_app
from backend.llm import ModelClient
from test_backend import fixture_config


class FakeGraph:
    ready=True
    state='ready'
    error=''
    info={'vertices':4,'edges':4}
    async def start(self):pass
    async def stop(self):pass
    async def call(self,name,args):return {'rows':[{'vertex_id':'10','name':'Alpha','value':14}],'metric':'total'}


def test_http_stream_health_and_input_validation(monkeypatch):
    async def run():
        c=fixture_config();counter=0
        def handler(request):
            nonlocal counter
            counter+=1
            if counter==1:message={'role':'assistant','content':'','tool_calls':[{'id':'t1','type':'function','function':{'name':'rank_vertices_weighted','arguments':'{"metric":"total","limit":1}'}}]}
            else:message={'role':'assistant','content':'# 结果\n\nAlpha：14。'}
            return httpx.Response(200,json={'choices':[{'message':message,'finish_reason':'stop'}]})
        monkeypatch.setattr('backend.agent.ModelClient',lambda config:ModelClient(config,httpx.MockTransport(handler)))
        app=create_app(c,FakeGraph())
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app),base_url='http://test') as client:
            health=(await client.get('/api/health')).json()
            assert health['status']=='ready'
            assert 'fixture-secret' not in json.dumps(health)
            assert (await client.post('/api/analyze',json={'question':'   '})).status_code==422
            response=await client.post('/api/analyze',json={'question':'最活跃公司'})
            assert response.status_code==200
            events=[json.loads(line) for line in response.text.splitlines()]
            assert events[0]['type']=='start' and events[-1]['type']=='report'
            assert any(e['type']=='evidence' for e in events)
            assert (await client.get('/api/health')).json()['busy'] is False
    asyncio.run(run())


def test_http_missing_model_and_loading():
    async def run():
        c=fixture_config();c['llm']['api_key']=''
        app=create_app(c,FakeGraph())
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app),base_url='http://test') as client:
            assert (await client.post('/api/analyze',json={'question':'排名'})).status_code==503
        graph=FakeGraph();graph.ready=False;graph.state='loading'
        app=create_app(fixture_config(),graph)
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app),base_url='http://test') as client:
            assert (await client.get('/api/health')).json()['status']=='loading'
            assert (await client.post('/api/analyze',json={'question':'排名'})).status_code==503
    asyncio.run(run())
