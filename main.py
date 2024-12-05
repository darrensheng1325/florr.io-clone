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
        
        self.peers = set()
        self.game = GameEngine()
        self.game.my_id = f"{host}:{port}"
        self.game.add_player(self.game.my_id)
        self.game.max_fps = 144
        
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
            
            # Only broadcast position if we're playing (not in title screen)
            if position and self.game.game_state == "playing":
                # Broadcast position to all peers
                self.broadcast_position(position)
            
            self.game.render()

            # Limit frame rate
            sleep(1 / self.game.max_fps)
        
        pygame.quit()
        sys.exit()
    
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
                print(f"Error receiving data: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python main.py <host> <port> [peer_host peer_port]")
        sys.exit(1)
    
    host = sys.argv[1]
    port = int(sys.argv[2])
    
    game = P2PGame(host, port)
    
    # If peer address is provided, connect to it
    if len(sys.argv) == 5:
        peer_host = sys.argv[3]
        peer_port = int(sys.argv[4])
        game.connect_to_peer(peer_host, peer_port)
    
    game.start() 