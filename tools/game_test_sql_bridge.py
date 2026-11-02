"""Loopback-only byte transport into a network-none owned SQL container.

Uses the existing native SQL test transport; no protocol results are invented.
Every docker-exec pipe is tracked and reaped on shutdown.
"""
import contextlib
import ipaddress
import os
import socket
import socketserver
import subprocess
import threading


class SqlBridge(socketserver.ThreadingTCPServer):
    allow_reuse_address = False
    daemon_threads = True

    def __init__(self, container, *, port=0, on_process=None):
        self.container, self.on_process = container, on_process
        self.processes = set()
        self.connections = set()
        self.guard = threading.Lock()
        super().__init__(('127.0.0.1', port), _Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    @property
    def port(self):
        return self.server_address[1]

    def close(self):
        self.shutdown()
        self.server_close()
        with self.guard:
            children = list(self.processes)
            connections = list(self.connections)
        for connection in connections:
            with contextlib.suppress(OSError):
                connection.shutdown(socket.SHUT_RDWR)
            with contextlib.suppress(OSError):
                connection.close()
        for process in children:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
        self.thread.join(timeout=10)


class _Handler(socketserver.BaseRequestHandler):
    def handle(self):
        process = subprocess.Popen(['docker', 'exec', '-i', self.server.container, 'bash', '-c',
            'exec 3<>/dev/tcp/127.0.0.1/1433; cat <&3 & reader=$!; cat >&3; kill "$reader" 2>/dev/null || true'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        with self.server.guard:
            self.server.processes.add(process)
            self.server.connections.add(self.request)
        if self.server.on_process:
            self.server.on_process(process)

        def receive():
            try:
                while True:
                    data = os.read(process.stdout.fileno(), 65536)
                    if not data:
                        break
                    self.request.sendall(data)
            except OSError:
                pass
            finally:
                with contextlib.suppress(OSError):
                    self.request.shutdown(socket.SHUT_WR)

        reader = threading.Thread(target=receive, daemon=True)
        reader.start()
        try:
            while True:
                data = self.request.recv(65536)
                if not data:
                    break
                process.stdin.write(data)
                process.stdin.flush()
        except (OSError, BrokenPipeError):
            pass
        finally:
            with contextlib.suppress(OSError):
                process.stdin.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            reader.join(timeout=5)
            with self.server.guard:
                self.server.processes.discard(process)
                self.server.connections.discard(self.request)
