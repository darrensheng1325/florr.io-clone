import socket
import json
import threading
import time

class GameServer:
    def __init__(self, port):
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.bind(('', port))
        self.server_socket.listen(5)
        
        self.clients = {}  # {client_id: socket}
        self.next_client_id = 0
        self.running = True
        
        print(f"Server started on port {port}")
        
        # Start broadcast thread
        self.broadcast_thread = threading.Thread(target=self.broadcast_loop)
        self.broadcast_thread.daemon = True
        self.broadcast_thread.start()
    
    def accept_connections(self):
        """Accept new client connections"""
        while self.running:
            try:
                client_socket, address = self.server_socket.accept()
                client_id = self.next_client_id
                self.next_client_id += 1
                
                # Send client their ID
                self.send_message(client_socket, {
                    'type': 'client_id',
                    'id': client_id
                })
                
                # Add client to list
                self.clients[client_id] = client_socket
                
                # Start client thread
                client_thread = threading.Thread(
                    target=self.handle_client,
                    args=(client_socket, client_id)
                )
                client_thread.daemon = True
                client_thread.start()
                
                print(f"Client {client_id} connected from {address}")
            except Exception as e:
                print(f"Error accepting connection: {e}")
    
    def handle_client(self, client_socket, client_id):
        """Handle messages from a client"""
        while self.running:
            try:
                data = client_socket.recv(4096)
                if not data:
                    break
                    
                message = json.loads(data.decode())
                self.broadcast_message(message, exclude=client_id)
            except Exception as e:
                print(f"Error handling client {client_id}: {e}")
                break
        
        # Remove disconnected client
        if client_id in self.clients:
            del self.clients[client_id]
            print(f"Client {client_id} disconnected")
    
    def broadcast_message(self, message, exclude=None):
        """Send message to all clients except excluded one"""
        for client_id, client_socket in self.clients.items():
            if client_id != exclude:
                try:
                    self.send_message(client_socket, message)
                except Exception as e:
                    print(f"Error broadcasting to client {client_id}: {e}")
    
    def send_message(self, client_socket, message):
        """Send message to specific client"""
        try:
            data = json.dumps(message).encode()
            client_socket.send(data)
        except Exception as e:
            print(f"Error sending message: {e}")

    def broadcast_monster_positions(self, monster_data):
        """Broadcast monster positions to all peers in chunks"""
        # Define type mapping - include ALL monster types
        type_map = {
            'Mouse': 'Mouse',
            'Cat': 'Cat',
            'Tank': 'Tank',
            'Bush': 'Bush',
            'Rock': 'Rock',
            'Ant': 'Ant',
            'Bee': 'Bee',
            'Boss': 'Boss',
            'Tree': 'Tree',  # Added Tree type
            'StaticMonster': 'StaticMonster'  # Added base static monster type
        }
        
        # Split monster data into smaller chunks
        chunk_size = 3
        monsters = list(monster_data.items())
        
        for i in range(0, len(monsters), chunk_size):
            chunk = dict(monsters[i:i + chunk_size])
            # Minimize data size but keep essential info intact
            compact_chunk = {}
            for monster_id, data in chunk.items():
                monster_type = data['type']
                if monster_type not in type_map:
                    print(f"Warning: Unknown monster type: {monster_type}")
                    monster_type = 'Mouse'  # Default to Mouse if type is unknown
                    
                compact_chunk[str(monster_id)] = {
                    'x': round(data['x'], 1),  # Keep some decimal precision
                    'y': round(data['y'], 1),
                    'h': int(data['health']),
                    't': type_map[monster_type]  # Use full monster type name
                }
            
            data = {
                't': 'mp',  # monster position message type
                'i': i // chunk_size,
                'n': (len(monsters) + chunk_size - 1) // chunk_size,
                'm': compact_chunk
            }
            
            # Send chunk to all peers
            for peer in self.clients:
                self.send_message(peer, data)
    
    
    def broadcast_loop(self):
        """Periodically broadcast game state to all clients"""
        while self.running:
            # Broadcast monster positions
            # You'll need to implement this based on your game state
            time.sleep(0.05)  # 20 times per second
    
    def close(self):
        """Shut down the server"""
        self.running = False
        for client_socket in self.clients.values():
            client_socket.close()
        self.server_socket.close() 

if __name__ == "__main__":
    server = GameServer(9000)
    server.accept_connections()
