from main_game_engine import GameEngine
from game_socket import GameSocket
from server import GameServer
import sys
import socket  # Import the full socket module
import threading
import json
from time import sleep, time
from monster import Mouse, Cat, Tank, Bush, Tree, Rock, Ant, Bee, Boss, Bird

class P2PGame:
    def __init__(self, host, port):
        # Create TCP socket
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.bind((host, port))
        self.socket.listen(5)  # Listen for incoming connections
        self.socket.settimeout(1.0)
        
        self.peers = {}  # Change to dict to store socket connections
        self.game = GameEngine()
        self.game.my_id = f"{host}:{port}"
        self.game.add_player(self.game.my_id)
        self.game.max_fps = 144
        
        # Add sync timers back
        self.last_monster_sync = 0
        self.monster_sync_interval = 0.05
        
        # Add valid monster types set
        self.valid_monster_types = {
            'Mouse', 'Cat', 'Tank', 'Bush', 'Rock', 
            'Ant', 'Bee', 'Boss', 'Tree', 'StaticMonster',
            'Bird'
        }
        
        # Start accept thread for incoming connections
        self.accept_thread = threading.Thread(target=self.accept_connections)
        self.accept_thread.daemon = True
        self.accept_thread.start()
    
    def accept_connections(self):
        """Accept incoming TCP connections"""
        while True:
            try:
                client_socket, address = self.socket.accept()
                # Start a new thread to handle this peer
                peer_thread = threading.Thread(
                    target=self.handle_peer,
                    args=(client_socket, address)
                )
                peer_thread.daemon = True
                peer_thread.start()
                self.peers[address] = client_socket
            except socket.timeout:
                continue
            except Exception as e:
                print(f"Error accepting connection: {e}")
    
    def handle_peer(self, peer_socket, address):
        """Handle communication with a peer"""
        try:
            while True:
                # TCP requires handling message boundaries
                data_len = int.from_bytes(peer_socket.recv(4), 'big')
                if not data_len:
                    break
                
                data = b''
                while len(data) < data_len:
                    chunk = peer_socket.recv(min(4096, data_len - len(data)))
                    if not chunk:
                        raise ConnectionError("Connection lost")
                    data += chunk
                
                message = json.loads(data.decode())
                self.handle_message(message, address)
        except Exception as e:
            print(f"Error handling peer {address}: {e}")
        finally:
            peer_socket.close()
            if address in self.peers:
                del self.peers[address]
    
    def broadcast_monster_positions(self, monster_data):
        """Broadcast monster positions to all peers"""
        try:
            # Validate and prepare monster data
            compact_data = {}
            for monster_id, data in monster_data.items():
                monster_type = data['type']
                if monster_type not in self.valid_monster_types:
                    print(f"Warning: Unknown monster type {monster_type}, skipping...")
                    continue
                
                compact_data[str(monster_id)] = {
                    'x': round(data['x'], 1),
                    'y': round(data['y'], 1),
                    'h': int(data['health']),
                    't': monster_type
                }
            
            if compact_data:  # Only send if we have valid monsters
                message = {
                    't': 'mp',  # monster positions
                    'm': compact_data
                }
                self.broadcast_message(message)
            
        except Exception as e:
            print(f"Error broadcasting monster positions: {e}")
            # Don't raise the exception, just log it
    
    def send_data(self, data, peer_socket):
        """Send data using TCP with message length prefix"""
        try:
            json_data = json.dumps(data).encode()
            length_prefix = len(json_data).to_bytes(4, 'big')
            peer_socket.sendall(length_prefix + json_data)
        except Exception as e:
            print(f"Error sending data: {e}")
            raise
    
    def broadcast_message(self, message):
        """Send message to all connected peers"""
        disconnected = []
        for address, peer_socket in self.peers.items():
            try:
                self.send_data(message, peer_socket)
            except Exception as e:
                print(f"Error sending to {address}: {e}")
                disconnected.append(address)
        
        # Clean up disconnected peers
        for address in disconnected:
            if address in self.peers:
                self.peers[address].close()
                del self.peers[address]
    
    def connect_to_peer(self, peer_host, peer_port):
        """Connect to a peer using TCP"""
        try:
            peer_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            peer_socket.connect((peer_host, peer_port))
            address = (peer_host, peer_port)
            self.peers[address] = peer_socket
            
            # Start handler thread for this peer
            peer_thread = threading.Thread(
                target=self.handle_peer,
                args=(peer_socket, address)
            )
            peer_thread.daemon = True
            peer_thread.start()
            
            # Send initial position
            position = self.game.get_player_position(self.game.my_id)
            self.send_data(position, peer_socket)
            
        except Exception as e:
            print(f"Error connecting to peer {peer_host}:{peer_port}: {e}")
            raise
    
    def start(self):
        # Start the receiving thread
        receive_thread = threading.Thread(target=self.receive_data)
        receive_thread.daemon = True
        receive_thread.start()
        
        # Main game loop
        running = True
        while running:
            current_time = time()
            
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
            
            # If host, broadcast monster positions periodically
            if self.game.is_host and self.game.game_state == "playing":
                if current_time - self.last_monster_sync >= self.monster_sync_interval:
                    monster_data = self.game.get_monster_positions()
                    if monster_data:
                        self.broadcast_monster_positions(monster_data)
                    self.last_monster_sync = current_time
            
            self.game.render()
            
            # Limit frame rate
            sleep(1 / self.game.max_fps)
    
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
    
    def broadcast_position(self, position):
        for peer in self.peers:
            self.send_data(position, peer)
    
    def receive_data(self):
        """Receive and process data from peers"""
        monster_chunks = {}
        
        while True:
            try:
                data, addr = self.socket.recvfrom(1024)
                data = json.loads(data.decode())
                
                # Add to peers if not already known
                if addr not in self.peers:
                    self.peers.add(addr)
                    self.game.add_player(data.get('player_id', ''))
                
                # Handle different message types
                msg_type = data.get('t', data.get('type'))
                
                if msg_type == 'mp':  # monster position
                    # Handle chunked monster position data
                    chunk_index = data.get('i', 0)
                    total_chunks = data.get('n', 1)
                    monster_data = data.get('m', {})
                    
                    # Expand data while preserving types
                    expanded_chunk = {}
                    for monster_id, mdata in monster_data.items():
                        expanded_chunk[int(monster_id)] = {
                            'x': float(mdata['x']),
                            'y': float(mdata['y']),
                            'health': int(mdata['h']),
                            'type': mdata['t'],  # Keep the full type name
                            'target_x': float(mdata['x']),
                            'target_y': float(mdata['y'])
                        }
                    
                    # Store chunk
                    if total_chunks > 1:
                        if addr not in monster_chunks:
                            monster_chunks[addr] = {}
                        monster_chunks[addr][chunk_index] = expanded_chunk
                        
                        # Check if we have all chunks
                        if len(monster_chunks[addr]) == total_chunks:
                            # Combine chunks
                            complete_data = {}
                            for i in range(total_chunks):
                                if i in monster_chunks[addr]:
                                    complete_data.update(monster_chunks[addr][i])
                        
                            # Update monster positions with complete data
                            self.game.update_monster_positions(complete_data)
                            
                            # Clear chunks
                            del monster_chunks[addr]
                    else:
                        # Single chunk, update directly
                        self.game.update_monster_positions(expanded_chunk)
                
                elif msg_type == 'ping':
                    # Send pong response
                    pong_data = {
                        'type': 'pong',
                        'player_id': self.game.my_id
                    }
                    self.send_data(pong_data, addr)
                
                elif 'x' in data and 'y' in data:
                    # Update player position
                    self.game.update_player(
                        data['player_id'],
                        data['x'],
                        data['y']
                    )
                    
            except socket.timeout:
                continue
            except Exception as e:
                print(f"Error receiving data: {e}")
                continue
    
    def handle_item_pickup(self, item):
        """Handle item pickup notification"""
        pickup_data = {
            'type': 'item_pickup',
            'player_id': self.game.my_id,
            'item_id': id(item)  # Use object id as unique identifier
        }
        self.broadcast_position(pickup_data)  # Use existing broadcast method

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