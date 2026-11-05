import asyncio
import httpx
from backend.app import create_app
from backend.config import load_config
from test_worker import fixture


def test_selection_lock_and_stale_analysis(tmp_path):
    path=fixture(tmp_path,directed=False)
    async def run():
        release=asyncio.Event()
        class Graph:
            ready=False;state='empty';error='';info={};progress={}
            async def start(self):pass
            async def stop(self):pass
            async def load(self,d):
                await release.wait()
                self.info={'id':d['id'],'name':d['name'],'directed':False}
                self.ready=True;self.state='ready'
        graph=Graph();app=create_app(load_config(),graph)
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app),base_url='http://test') as c:
            assert (await c.get('/api/health')).json()['status']=='empty'
            assert (await c.post('/api/datasets/load',json={'path':str(path)})).status_code==200
            assert (await c.get('/api/health')).json()['busy']
            assert (await c.post('/api/datasets/load',json={'path':str(path)})).status_code==409
            release.set()
            await asyncio.sleep(.01)
            assert (await c.get('/api/health')).json()['status']=='ready'
            assert (await c.post('/api/datasets/load',json={'path':str(tmp_path/'absent.json')})).status_code==400
            assert graph.ready
            assert (await c.post('/api/analyze',json={'question':'rank','dataset_id':'old'})).status_code==409
    asyncio.run(run())


def test_native_load_failure_exposed_by_health_without_partial_graph(tmp_path):
    import json
    from backend.graph_worker import GraphWorker
    path=fixture(tmp_path)
    d=json.loads(path.read_text());d['edges']['columns']['weight']='absent_column';path.write_text(json.dumps(d))
    async def run():
        worker=GraphWorker(load_config());app=create_app(load_config(),worker)
        try:
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app),base_url='http://test') as client:
                response=await client.post('/api/datasets/load',json={'path':str(path)})
                assert response.status_code==200
                for _ in range(500):
                    health=(await client.get('/api/health')).json()
                    if health['status']=='error':break
                    await asyncio.sleep(.01)
                assert health['status']=='error' and not health['busy']
                assert health['dataset']=={} and 'missing Parquet column' in health['error']
                assert (await client.post('/api/analyze',json={'question':'rank'})).status_code==503
        finally:await worker.stop()
    asyncio.run(run())
