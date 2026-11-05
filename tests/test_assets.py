import asyncio
import hashlib
import httpx
from backend.app import create_app
from backend.config import ROOT, load_config


def test_html_versions_assets_and_disables_stale_cache():
    async def run():
        app=create_app(load_config())
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app),base_url='http://test') as c:
            page=await c.get('/')
            assert page.headers['cache-control']=='no-store'
            for name in ('app.js','styles.css'):
                digest=hashlib.sha256((ROOT/'frontend'/name).read_bytes()).hexdigest()[:12]
                url=f'/assets/{name}?v={digest}'
                assert url in page.text
                response=await c.get(url)
                assert response.status_code==200
                assert response.headers['cache-control']=='no-store'
            assert 'dataset-path' in page.text
    asyncio.run(run())
