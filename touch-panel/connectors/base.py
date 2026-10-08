import json
import urllib.request
import urllib.error
import ssl

class BaseConnector:
    """Base class for all API connectors."""
    
    def __init__(self, name, config=None):
        self.name = name
        self.config = config or {}

    def fetch_data(self, endpoint=None, params=None):
        """Fetch data from the API. Must be implemented by subclasses."""
        raise NotImplementedError("Subclasses must implement fetch_data")

    def _http_get(self, url, headers=None, timeout=5, verify_ssl=True):
        """Helper method for HTTP GET requests with timeout and SSL handling."""
        req_headers = {"User-Agent": "Mozilla/5.0 (RPi TouchPanel)"}
        if headers:
            req_headers.update(headers)
            
        req = urllib.request.Request(url, headers=req_headers)
        
        ctx = None
        if not verify_ssl:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            
        try:
            with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
                data = resp.read().decode('utf-8')
                return json.loads(data)
        except urllib.error.HTTPError as e:
            return {"error": True, "message": f"HTTP Error {e.code}: {e.reason}"}
        except urllib.error.URLError as e:
            return {"error": True, "message": f"URL Error: {e.reason}"}
        except Exception as e:
            return {"error": True, "message": f"Error: {str(e)}"}

    def _http_post(self, url, headers=None, payload=None, timeout=5, verify_ssl=True):
        """Helper method for HTTP POST requests with timeout and SSL handling."""
        req_headers = {"User-Agent": "Mozilla/5.0 (RPi TouchPanel)"}
        if headers:
            req_headers.update(headers)
            
        data_bytes = None
        if payload is not None:
            data_bytes = json.dumps(payload).encode('utf-8')
            req_headers["Content-Type"] = "application/json"
            
        req = urllib.request.Request(url, data=data_bytes, headers=req_headers, method="POST")
        
        ctx = None
        if not verify_ssl:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            
        try:
            with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
                data = resp.read().decode('utf-8')
                return json.loads(data) if data else {"success": True}
        except urllib.error.HTTPError as e:
            return {"error": True, "message": f"HTTP Error {e.code}: {e.reason}"}
        except urllib.error.URLError as e:
            return {"error": True, "message": f"URL Error: {e.reason}"}
        except Exception as e:
            return {"error": True, "message": f"Error: {str(e)}"}

