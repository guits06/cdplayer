import urllib.request
import urllib.parse
import json
from .base import BaseConnector

class CDPlayerConnector(BaseConnector):
    """Connector for the local CD Player API on port 8000."""
    
    def __init__(self, config=None):
        super().__init__("cdplayer", config or {})
        self.base_url = self.config.get("url", "http://localhost:8000")

    def fetch_data(self, endpoint="status", params=None):
        url = f"{self.base_url}/api/{endpoint}"
        return self._http_get(url, timeout=3)

    def post_action(self, action, payload=None):
        """Send POST action to CD player API (e.g. play, pause, stop, eject, next, prev, pc_audio_toggle)."""
        if action == "pc_audio_toggle":
            status = self.fetch_data("status")
            if status and status.get("pc_audio_active"):
                url = f"{self.base_url}/api/pc_audio/disable"
            else:
                url = f"{self.base_url}/api/pc_audio/enable"
        elif action in ("pc_audio_enable", "pc_audio_disable"):
            action_name = "enable" if action == "pc_audio_enable" else "disable"
            url = f"{self.base_url}/api/pc_audio/{action_name}"
        else:
            url = f"{self.base_url}/api/{action}"

        data = None
        headers = {}
        if payload:
            data = json.dumps(payload).encode('utf-8')
            headers['Content-Type'] = 'application/json'
        
        req = urllib.request.Request(url, data=data, headers=headers, method='POST')
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                res_text = resp.read().decode('utf-8')
                return json.loads(res_text) if res_text else {"status": "ok"}
        except Exception as e:
            return {"error": True, "message": str(e)}
