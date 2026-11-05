"""Load and inspect any external dataset without a model API call."""
import argparse,asyncio,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from backend.config import load_config
from backend.datasets import descriptor
from backend.graph_worker import GraphWorker
async def main(path):
 w=GraphWorker(load_config())
 try:
  await w.load(descriptor(path))
  print(json.dumps(w.info,ensure_ascii=False,indent=2))
  print(json.dumps(await w.call('rank_vertices_weighted',{'metric':'total' if w.info['directed'] else 'strength','limit':5}),ensure_ascii=False,indent=2))
 finally:await w.stop()
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('descriptor');args=p.parse_args();asyncio.run(main(args.descriptor))
