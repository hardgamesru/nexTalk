from http.server import BaseHTTPRequestHandler, HTTPServer
import json


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/analyze":
            self.send_response(404)
            self.end_headers()
            return

        content_length = int(self.headers.get("Content-Length", 0))
        raw_body = self.rfile.read(content_length).decode("utf-8")
        data = json.loads(raw_body) if raw_body else {}

        response = {
            "status": "ok",
            "summary": f"Stub analysis for file_id={data.get('file_id')}",
            "keywords": ["stub", "test", "analysis"]
        }

        encoded = json.dumps(response).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


if __name__ == "__main__":
    server = HTTPServer(("127.0.0.1", 5000), Handler)
    print("AI service started on http://127.0.0.1:5000")
    server.serve_forever()