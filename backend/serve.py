from __future__ import annotations

import socket
import sys
import threading
import webbrowser
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from backend.config import load_config


def main():
    import uvicorn
    config = load_config()
    host, port = config["server"]["host"], config["server"]["port"]
    with socket.socket() as probe:
        try:
            probe.bind((host, port))
        except OSError:
            print(f"Port {port} is already in use. Close the previous KHyper terminal or run start.ps1 -Port 8766.")
            return 1
    url = f"http://{host}:{port}"
    print(f"\nKHyper: {url}\nSelect an external dataset descriptor in the page. Ctrl+C stops the service and C++ worker.\n", flush=True)
    if "--no-browser" not in sys.argv:
        timer = threading.Timer(2, lambda: webbrowser.open(url))
        timer.daemon = True
        timer.start()
    uvicorn.run("backend.app:app", host=host, port=port, access_log=False, log_level="info")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
