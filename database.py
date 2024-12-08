import sqlite3
from contextlib import contextmanager
from item import Item
import pygame

class GameDatabase:
    def __init__(self, db_path="game.db"):
        self.db_path = db_path
        self.init_database()

    @contextmanager
    def get_connection(self):
        conn = sqlite3.connect(self.db_path)
        try:
            yield conn
        finally:
            conn.close()

    def init_database(self):
        """Initialize the database schema"""
        with self.get_connection() as conn:
            cursor = conn.cursor()
            
            # Create players table
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS players (
                    player_id TEXT PRIMARY KEY,
                    health INTEGER DEFAULT 100,
                    x INTEGER DEFAULT 800,
                    y INTEGER DEFAULT 600
                )
            ''')

            # Create inventory table
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS inventory (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    player_id TEXT,
                    item_name TEXT,
                    item_color TEXT,
                    damage INTEGER,
                    radius INTEGER,
                    max_health INTEGER,
                    image_path TEXT,
                    count INTEGER DEFAULT 1,
                    FOREIGN KEY (player_id) REFERENCES players (player_id)
                )
            ''')

            # Create equipped_petals table
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS equipped_petals (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    player_id TEXT,
                    slot_index INTEGER,
                    item_name TEXT,
                    item_color TEXT,
                    damage INTEGER,
                    radius INTEGER,
                    max_health INTEGER,
                    image_path TEXT,
                    FOREIGN KEY (player_id) REFERENCES players (player_id)
                )
            ''')
            
            conn.commit()

    def save_player(self, player_id, data):
        """Save or update player data"""
        with self.get_connection() as conn:
            cursor = conn.cursor()
            
            # Save player basic info
            cursor.execute('''
                INSERT OR REPLACE INTO players (player_id, health, x, y)
                VALUES (?, ?, ?, ?)
            ''', (player_id, data['health'], data['x'], data['y']))

            # Clear existing inventory for this player
            cursor.execute('DELETE FROM inventory WHERE player_id = ?', (player_id,))
            
            # Save inventory
            for item_name, item_data in data['inventory'].items():
                item = item_data['item']
                cursor.execute('''
                    INSERT INTO inventory 
                    (player_id, item_name, item_color, damage, radius, max_health, count, image_path)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                ''', (player_id, item.name, str(item.color), item.damage, 
                     item.radius, item.max_health, item_data['count'], item.image_path))

            # Clear existing equipped petals
            cursor.execute('DELETE FROM equipped_petals WHERE player_id = ?', (player_id,))
            
            # Save equipped petals
            for i, petal in enumerate(data['equipped_petals']):
                if petal is not None:
                    cursor.execute('''
                        INSERT INTO equipped_petals 
                        (player_id, slot_index, item_name, item_color, damage, radius, max_health, image_path)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                    ''', (player_id, i, petal.name, str(petal.color), petal.damage, 
                         petal.radius, petal.max_health, petal.image_path))

            conn.commit()

    def load_player(self, player_id):
        """Load player data from database"""
        with self.get_connection() as conn:
            cursor = conn.cursor()
            
            # Load basic player info
            cursor.execute('SELECT health, x, y FROM players WHERE player_id = ?', (player_id,))
            result = cursor.fetchone()
            
            if not result:
                return None
                
            player_data = {
                'health': result[0],
                'x': result[1],
                'y': result[2],
                'angle': 0,
                'inventory': {},
                'equipped_petals': [None] * 5,
                'image': None
            }

            # Load inventory
            cursor.execute('''
                SELECT item_name, item_color, damage, radius, max_health, count, image_path 
                FROM inventory WHERE player_id = ?
            ''', (player_id,))
            
            for row in cursor.fetchall():
                item_name, color_str, damage, radius, max_health, count, image_path = row
                color = eval(color_str)  # Convert string representation back to tuple
                item = Item(item_name, color, damage, radius, max_health, image_path)
                
                # Load the image if path exists
                if image_path:
                    try:
                        item.image = pygame.image.load(image_path).convert_alpha()
                    except Exception as e:
                        print(f"Failed to load image for {item_name}: {e}")
                
                player_data['inventory'][item_name] = {'item': item, 'count': count}

            # Load equipped petals
            cursor.execute('''
                SELECT slot_index, item_name, item_color, damage, radius, max_health, image_path 
                FROM equipped_petals WHERE player_id = ?
                ORDER BY slot_index
            ''', (player_id,))
            
            for row in cursor.fetchall():
                slot_index, item_name, color_str, damage, radius, max_health, image_path = row
                color = eval(color_str)
                petal = Item(item_name, color, damage, radius, max_health, image_path)
                
                # Load the image if path exists
                if image_path:
                    try:
                        petal.image = pygame.image.load(image_path).convert_alpha()
                    except Exception as e:
                        print(f"Failed to load image for {item_name}: {e}")
                
                player_data['equipped_petals'][slot_index] = petal

            return player_data 