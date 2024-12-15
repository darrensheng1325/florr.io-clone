import socket
import json
import threading
import time

class GameSocket:
    def __init__(self, game_engine):
        self.game_engine = game_engine
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.connected = False
        self.is_host = False
        self.client_id = None
        
    # ... rest of the code stays the same ... 