from main_game_engine import GameEngine
import sys
import socket
import threading
import json
from time import sleep

class P2PGame:
    def __init__(self, host, port):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((host, port))
        self.socket.settimeout(1.0)  # Set socket timeout for ping
        
        self.peers = set()
        self.game = GameEngine()
        self.game.my_id = f"{host}:{port}"
        self.game.add_player(self.game.my_id)
        self.game.max_fps = 144
        
        self.host = host
        self.port = port
    
    def test_peer_connection(self, peer_host, peer_port):
        """Test connection to peer with ping-pong"""
        try:
            # Send ping
            ping_data = {
                'type': 'ping',
                'player_id': self.game.my_id
            }
            self.send_data(ping_data, (peer_host, peer_port))
            
            # Wait for pong
            try:
                data, addr = self.socket.recvfrom(1024)
                data = json.loads(data.decode())
                
                if data.get('type') == 'pong':
                    return True
            except socket.timeout:
                return False
                
        except Exception as e:
            print(f"Connection test failed: {e}")
            return False
    
    def start(self):
        # Start the receiving thread
        receive_thread = threading.Thread(target=self.receive_data)
        receive_thread.daemon = True
        receive_thread.start()
        
        # Main game loop
        running = True
        while running:
            # Handle local player movement
            position = self.game.handle_local_input()
            
            # Check if we need to test connection to a peer
            if hasattr(self.game, 'test_connection') and self.game.test_connection:
                if self.game.connect_ip:
                    try:
                        peer_host, peer_port = self.game.connect_ip.split(':')
                        peer_port = int(peer_port)
                        if self.test_peer_connection(peer_host, peer_port):
                            self.connect_to_peer(peer_host, peer_port)
                        else:
                            print("Connection test failed")
                    except Exception as e:
                        print(f"Error connecting to peer: {e}")
                self.game.test_connection = False  # Clear the test flag
            
            # Only broadcast position if we're playing
            if position and self.game.game_state == "playing":
                self.broadcast_position(position)
            
            self.game.render()
            
            # Limit frame rate
            sleep(1 / self.game.max_fps)
    
    def connect_to_peer(self, peer_host, peer_port):
        peer_address = (peer_host, peer_port)
        self.peers.add(peer_address)
        # Send initial position to new peer
        position = self.game.get_player_position(self.game.my_id)
        self.send_data(position, peer_address)
    
    def broadcast_position(self, position):
        for peer in self.peers:
            self.send_data(position, peer)
    
    def send_data(self, data, address):
        try:
            self.socket.sendto(json.dumps(data).encode(), address)
        except Exception as e:
            print(f"Error sending data: {e}")
    
    def receive_data(self):
        while True:
            try:
                data, addr = self.socket.recvfrom(1024)
                data = json.loads(data.decode())
                
                # Handle ping requests
                if data.get('type') == 'ping':
                    # Send pong response
                    pong_data = {
                        'type': 'pong',
                        'player_id': self.game.my_id
                    }
                    self.send_data(pong_data, addr)
                    continue
                
                # Update player position
                self.game.update_player(
                    data['player_id'],
                    data['x'],
                    data['y']
                )
                
                # Add to peers if not already known
                if addr not in self.peers:
                    self.peers.add(addr)
                    self.game.add_player(data['player_id'])
                    
            except Exception as e:
                if not isinstance(e, socket.timeout):
                    print(f"Error receiving data: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python main.py <host> <port> [peer_host peer_port]")
        sys.exit(1)
    
    host = sys.argv[1]
    port = int(sys.argv[2])
    
    game = P2PGame(host, port)
    
    # Note: Command line peer connection is now optional since we can use the input box
    if len(sys.argv) == 5:
        peer_host = sys.argv[3]
        peer_port = int(sys.argv[4])
        game.connect_to_peer(peer_host, peer_port)
    
    game.start() 