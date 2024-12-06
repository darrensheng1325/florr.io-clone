import pygame
import json
import math
import random
import sys
import time
from cairosvg import svg2png
from io import BytesIO
from PIL import Image
from item import Item, DroppedItem, RockItem
from monster import Mouse, Cat, Tank, Bush, Tree, Rock, Ant, Bee
from database import GameDatabase

class GameEngine:
    def __init__(self, width=4800, height=1200, initial_monster_count=5):
        pygame.init()
        self.screen = pygame.display.set_mode((800, 600))
        pygame.display.set_caption("P2P Game")
        self.clock = pygame.time.Clock()
        
        # Add game state
        self.game_state = "loading"  # Can be "loading", "title", or "playing"
        self.loading_progress = 0  # Track loading progress
        
        # Show initial loading screen
        self.render_loading_screen("Initializing game...")
        
        # World dimensions
        self.world_width = width
        self.world_height = height
        self.loading_progress = 10
        self.render_loading_screen("Loading images...")
        
        # Load player image with specific size
        self.player_image = self.load_svg_image("player.svg", size=(40, 40))  # Smaller than bee (80x80)
        self.loading_progress = 20
        self.render_loading_screen("Creating items...")
        
        # Define possible items with different health values
        self.possible_items = [
            Item("Basic Petal", (255, 255, 255), damage=10, radius=10, max_health=100),
            Item("Fire Petal", (255, 100, 0), damage=15, radius=12, max_health=80),
            Item("Ice Petal", (0, 255, 255), damage=12, radius=11, max_health=120),
            Item("Poison Petal", (0, 255, 0), damage=8, radius=13, max_health=150),
            RockItem()
        ]
        
        # Petal properties
        self.petal_count = 5
        self.base_orbit_radius = 50  # Default radius
        self.max_orbit_radius = 100  # Maximum expansion
        self.min_orbit_radius = 30   # Minimum contraction
        self.orbit_radius = self.base_orbit_radius  # Current radius
        self.orbit_speed = 0.05
        self.radius_change_speed = 3  # Speed of expansion/contraction
        
        self.players = {}
        self.my_id = None
        
        # Define monster types and their spawn weights
        self.monster_types = {
            Ant: 25,     # 25% chance for ant
            Bee: 25,     # 25% chance for bee
            Mouse: 15,   # 15% chance for mouse
            Cat: 15,     # 15% chance for cat
            Tank: 10,    # 10% chance for tank
            Bush: 5,     # 5% chance for bush
            Tree: 3,     # 3% chance for tree
            Rock: 2      # 2% chance for rock
        }
        
        # Add zone definitions (side by side, each 1600 pixels wide)
        self.zones = {
            'easy': {
                'color': (28, 168, 99),  # Light green
                'rect': pygame.Rect(0, 0, width // 3, height),  # Now 1600 pixels wide
                'monster_multiplier': 1.0,  # Base difficulty
                'spawn_weights': {
                    Ant: 45,     # Mostly ants
                    Bee: 35,     # and bees
                    Bush: 10,
                    Tree: 5,
                    Rock: 5
                }
            },
            'medium': {
                'color': (240, 255, 110),  # Peach
                'rect': pygame.Rect(width // 3, 0, width // 3, height),  # Now 1600 pixels wide
                'monster_multiplier': 1.5,  # 50% stronger
                'spawn_weights': {
                    Mouse: 35,    # More mice
                    Cat: 25,      # Some cats
                    Bee: 15,      # Few bees
                    Tank: 10,
                    Bush: 8,
                    Tree: 4,
                    Rock: 3
                }
            },
            'hard': {
                'color': (248, 58, 41),  # Salmon
                'rect': pygame.Rect(2 * width // 3, 0, width // 3, height),  # Now 1600 pixels wide
                'monster_multiplier': 2.0,  # Double strength
                'spawn_weights': {
                    Cat: 35,      # Mostly cats
                    Mouse: 25,    # and mice
                    Tank: 25,     # and tanks
                    Bush: 8,
                    Tree: 4,
                    Rock: 3
                }
            }
        }
        
        # Increase initial monster count to account for larger map
        initial_monster_count *= 3  # Triple the monsters for triple the width
        
        # Create initial monsters
        self.monsters = []
        # First add static monsters in good positions
        self.loading_progress = 40
        self.render_loading_screen("Creating static monsters...")
        self.add_static_monsters(initial_monster_count * 2)  # More static monsters
        
        self.loading_progress = 60
        self.render_loading_screen("Creating mobile monsters...")
        # Then add mobile monsters
        for _ in range(initial_monster_count):
            self.monsters.append(self.create_mobile_monster())
        
        self.loading_progress = 80
        self.render_loading_screen("Loading monster images...")
        
        # Show inventory flag
        self.show_inventory = False
        
        # Add dropped items list
        self.dropped_items = []
        
        # Load monster images with higher DPI
        self.mouse_image = self.load_svg_image("mouse.svg", size=(30, 30))
        self.cat_image = self.load_svg_image("cat.svg", size=(50, 50))
        self.bee_image = self.load_svg_image("bee.svg", size=(80, 80), dpi=300)  # Higher DPI for bee
        
        # Pass images to monster classes
        Mouse.image = self.mouse_image
        Cat.image = self.cat_image
        Bee.image = self.bee_image
        
        # Add broken petal tracking
        self.broken_petals = {}  # {slot_index: (item, respawn_time)}
        self.petal_respawn_time = 2  # Seconds until respawn
        
        self.loading_progress = 90
        self.render_loading_screen("Initializing database...")
        
        # Add database
        self.db = GameDatabase()
        
        # Add after other initializations
        self.max_health = 100
        self.heal_rate = 5  # Health points per second
        self.last_heal_time = time.time()
        
        self.loading_progress = 100
        self.render_loading_screen("Done!")
        pygame.time.wait(500)  # Show 100% for half a second
        
        # Switch to title screen
        self.game_state = "title"
        
        # Add after other initializations
        self.input_text = ""
        self.input_active = False
        self.connect_ip = None  # Will store the IP:port to connect to
    
    def render_loading_screen(self, message):
        """Render the loading screen with progress bar"""
        # Fill background
        self.screen.fill((28, 168, 99))  # Light green background
        
        # Create fonts
        font_large = pygame.font.Font(None, 74)
        font_small = pygame.font.Font(None, 36)
        
        # Draw title
        title_text = font_large.render("florr.io", True, (255, 255, 255))
        title_rect = title_text.get_rect(center=(400, 150))
        self.screen.blit(title_text, title_rect)
        
        # Draw loading message
        loading_text = font_small.render(message, True, (255, 255, 255))
        loading_rect = loading_text.get_rect(center=(400, 250))
        self.screen.blit(loading_text, loading_rect)
        
        # Draw progress bar
        bar_width = 400
        bar_height = 20
        bar_rect = pygame.Rect(200, 300, bar_width, bar_height)
        pygame.draw.rect(self.screen, (255, 255, 255), bar_rect, 2)  # Border
        
        # Fill progress
        fill_width = int(bar_width * (self.loading_progress / 100))
        if fill_width > 0:
            fill_rect = pygame.Rect(200, 300, fill_width, bar_height)
            pygame.draw.rect(self.screen, (255, 255, 255), fill_rect)
        
        # Draw percentage
        percent_text = font_small.render(f"{self.loading_progress}%", True, (255, 255, 255))
        percent_rect = percent_text.get_rect(center=(400, 350))
        self.screen.blit(percent_text, percent_rect)
        
        pygame.display.flip()
    
    def add_static_monsters(self, count):
        static_types = [Bush, Tree, Rock]
        min_distance = 100  # Minimum distance between static monsters
        
        for _ in range(count):
            placed = False
            attempts = 0
            while not placed and attempts < 50:
                x = random.randint(50, self.world_width - 50)
                y = random.randint(50, self.world_height - 50)
                
                # Check distance from other monsters
                too_close = False
                for monster in self.monsters:
                    dx = monster.x - x
                    dy = monster.y - y
                    if math.sqrt(dx*dx + dy*dy) < min_distance:
                        too_close = True
                        break
                
                if not too_close:
                    # Get zone and its weights
                    zone = self.get_zone(x, y)
                    zone_data = self.zones[zone]
                    
                    # Filter weights for static monsters only
                    static_weights = {k: v for k, v in zone_data['spawn_weights'].items() 
                                   if k in static_types}
                    weights = list(static_weights.values())
                    monster_type = random.choices(list(static_weights.keys()), 
                                               weights=weights)[0]
                    
                    # Create monster with zone multiplier
                    monster = monster_type(x, y)
                    multiplier = zone_data['monster_multiplier']
                    monster.health *= multiplier
                    
                    self.monsters.append(monster)
                    placed = True
                
                attempts += 1
    
    def create_mobile_monster(self):
        x = random.randint(50, self.world_width - 50)
        y = random.randint(50, self.world_height - 50)
        
        # Determine which zone the monster is in
        zone = self.get_zone(x, y)
        zone_data = self.zones[zone]
        
        # Keep trying until we get a valid spawn location for the monster type
        attempts = 0
        while attempts < 50:
            # Choose monster type based on zone weights
            weights = list(zone_data['spawn_weights'].values())
            monster_type = random.choices(
                list(zone_data['spawn_weights'].keys()),
                weights=weights
            )[0]
            
            # If it's a Mouse or Cat, make sure it spawns in medium or hard zone
            if (monster_type in [Mouse, Cat] and zone == 'easy'):
                attempts += 1
                continue
            
            # Create monster and adjust its stats based on zone
            monster = monster_type(x, y)
            multiplier = zone_data['monster_multiplier']
            monster.health *= multiplier
            if not hasattr(monster, 'is_passive') or not monster.is_passive:
                monster.damage *= multiplier  # Only multiply damage for aggressive mobs
            
            return monster
        
        # If we couldn't find a valid spawn after 50 attempts, create a passive mob
        return Ant(x, y)  # Default to spawning an ant
    
    def get_zone(self, x, y):
        for zone_name, zone_data in self.zones.items():
            if zone_data['rect'].collidepoint(x, y):
                return zone_name
        return 'easy'  # Default to easy zone
    
    def add_player(self, player_id, x=800, y=600):
        # Try to load player data from database
        player_data = self.db.load_player(player_id)
        
        if player_data:
            player_data['image'] = self.player_image  # Set the player image
            self.players[player_id] = player_data
        else:
            # Create new player with default values
            basic_petal = Item("Basic Petal", (255, 255, 255), damage=10, radius=10, max_health=100)
            self.players[player_id] = {
                'x': x,
                'y': y,
                'image': self.player_image,
                'angle': 0,
                'health': 100,
                'inventory': {},
                'equipped_petals': [basic_petal for _ in range(self.petal_count)]
            }
            # Save new player to database
            self.db.save_player(player_id, self.players[player_id])
    
    def update_player(self, player_id, x, y):
        if player_id in self.players:
            self.players[player_id]['x'] = x
            self.players[player_id]['y'] = y
    
    def handle_local_input(self):
        if self.game_state == "loading":
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    pygame.quit()
                    sys.exit()
            return None
        elif self.game_state == "title":
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    pygame.quit()
                    sys.exit()
                elif event.type == pygame.MOUSEBUTTONDOWN:
                    # Check for input box click
                    if hasattr(self, 'input_box_rect') and self.input_box_rect.collidepoint(event.pos):
                        self.input_active = True
                    else:
                        self.input_active = False
                    
                    # Check for start button click
                    if hasattr(self, 'start_button_rect') and self.start_button_rect.collidepoint(event.pos):
                        if self.input_text.strip():  # If there's text in the input box
                            self.connect_ip = self.input_text.strip()
                        self.start_game()
                        return None
                elif event.type == pygame.KEYDOWN and self.input_active:
                    if event.key == pygame.K_RETURN:
                        self.input_active = False
                    elif event.key == pygame.K_BACKSPACE:
                        self.input_text = self.input_text[:-1]
                    else:
                        self.input_text += event.unicode
            return None
        
        current_time = time.time()
        
        # Handle healing
        if self.my_id in self.players:
            player = self.players[self.my_id]
            if player['health'] < self.max_health:
                # Calculate time since last heal
                time_diff = current_time - self.last_heal_time
                
                # Calculate heal amount based on time passed
                heal_amount = self.heal_rate * time_diff
                
                # Apply healing
                player['health'] = min(player['health'] + heal_amount, self.max_health)
            
            self.last_heal_time = current_time
        
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                pygame.quit()
                sys.exit()
            elif event.type == pygame.MOUSEBUTTONDOWN:
                # Check if inventory button was clicked
                mouse_pos = pygame.mouse.get_pos()
                if 10 <= mouse_pos[0] <= 70 and 520 <= mouse_pos[1] <= 580:  # Inventory button bounds
                    self.show_inventory = not self.show_inventory
                elif self.show_inventory:
                    self.handle_inventory_click(event.pos)
                elif event.button == 1:  # Left click
                    self.orbit_radius = min(self.orbit_radius + self.radius_change_speed, self.max_orbit_radius)
                elif event.button == 3:  # Right click
                    self.orbit_radius = max(self.orbit_radius - self.radius_change_speed, self.min_orbit_radius)
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_i:
                    self.show_inventory = not self.show_inventory

        if not self.show_inventory:  # Only handle movement when inventory is closed
            keys = pygame.key.get_pressed()
            if self.my_id in self.players:
                if keys[pygame.K_LEFT]:
                    self.players[self.my_id]['x'] -= 5
                if keys[pygame.K_RIGHT]:
                    self.players[self.my_id]['x'] += 5
                if keys[pygame.K_UP]:
                    self.players[self.my_id]['y'] -= 5
                if keys[pygame.K_DOWN]:
                    self.players[self.my_id]['y'] += 5
                    
                # Handle petal expansion/contraction with space/shift
                if keys[pygame.K_SPACE]:
                    self.orbit_radius = min(self.orbit_radius + self.radius_change_speed, self.max_orbit_radius)
                elif keys[pygame.K_LSHIFT] or keys[pygame.K_RSHIFT]:
                    self.orbit_radius = max(self.orbit_radius - self.radius_change_speed, self.min_orbit_radius)
                else:
                    # Gradually return to base radius when no key is pressed
                    if self.orbit_radius > self.base_orbit_radius:
                        self.orbit_radius = max(self.orbit_radius - self.radius_change_speed/2, self.base_orbit_radius)
                    elif self.orbit_radius < self.base_orbit_radius:
                        self.orbit_radius = min(self.orbit_radius + self.radius_change_speed/2, self.base_orbit_radius)
                
        if self.my_id in self.players:
            # Save player state periodically
            self.db.save_player(self.my_id, self.players[self.my_id])
        
        return self.get_player_position(self.my_id)
    
    def get_player_position(self, player_id):
        if player_id in self.players:
            return {
                'player_id': player_id,
                'x': self.players[player_id]['x'],
                'y': self.players[player_id]['y']
            }
        return None
    
    def add_item_to_inventory(self, player, item):
        if item is None:
            return
        
        # Create a new instance of the item to ensure separate health values
        new_item = Item(item.name, item.color, item.damage, item.radius, item.max_health)
        if item.name in player['inventory']:
            player['inventory'][item.name]['count'] += 1
        else:
            player['inventory'][item.name] = {'item': new_item, 'count': 1}
    
    def render_inventory(self):
        inventory_surface = pygame.Surface((600, 400))
        inventory_surface.fill((99, 255, 133))
        
        player = self.players[self.my_id]
        font = pygame.font.Font(None, 36)
        small_font = pygame.font.Font(None, 24)
        
        # Draw equipped petals
        text = font.render("Equipped Petals:", True, (255, 255, 255))
        inventory_surface.blit(text, (20, 20))
        
        for i, petal in enumerate(player['equipped_petals']):
            if petal is not None:  # Check if petal is not None
                pygame.draw.circle(inventory_surface, petal.color, (50 + i * 60, 70), 20)
        
        # Draw inventory items
        text = font.render("Inventory:", True, (255, 255, 255))
        inventory_surface.blit(text, (20, 120))
        
        # Render stacked items
        i = 0
        for item_name, item_data in player['inventory'].items():
            pos_x = 50 + (i % 8) * 60
            pos_y = 170 + (i // 8) * 60
            
            # Draw item
            pygame.draw.circle(inventory_surface, item_data['item'].color, (pos_x, pos_y), 20)
            
            # Draw count
            count_text = small_font.render(str(item_data['count']), True, (255, 255, 255))
            count_rect = count_text.get_rect(bottomright=(pos_x + 25, pos_y + 25))
            inventory_surface.blit(count_text, count_rect)
            
            # Handle clicking on inventory items
            mouse_pos = pygame.mouse.get_pos()
            inventory_pos = (mouse_pos[0] - 100, mouse_pos[1] - 100)
            
            if pygame.mouse.get_pressed()[0]:  # Left click
                distance = math.sqrt((inventory_pos[0] - pos_x)**2 + (inventory_pos[1] - pos_y)**2)
                if distance < 20:
                    slot = self.show_slot_selection()
                    if slot is not None:
                        # Equip item and decrease stack count
                        old_petal = player['equipped_petals'][slot]
                        player['equipped_petals'][slot] = item_data['item']
                        item_data['count'] -= 1
                        
                        # Remove item from inventory if count reaches 0
                        if item_data['count'] <= 0:
                            del player['inventory'][item_name]
                        
                        # Add old petal back to inventory
                        self.add_item_to_inventory(player, old_petal)
                        
                        self.show_inventory = False
            i += 1
        
        self.screen.blit(inventory_surface, (100, 100))
        pygame.display.flip()
    
    def show_slot_selection(self):
        font = pygame.font.Font(None, 36)
        text = font.render("Select slot (1-5):", True, (255, 255, 255))
        self.screen.blit(text, (300, 300))
        pygame.display.flip()
        
        while True:
            for event in pygame.event.get():
                if event.type == pygame.KEYDOWN:
                    if event.key in [pygame.K_1, pygame.K_2, pygame.K_3, pygame.K_4, pygame.K_5]:
                        return int(event.unicode) - 1
                    if event.key == pygame.K_ESCAPE:
                        return None
    
    def render(self):
        if self.game_state == "loading":
            return  # Loading screen is handled during initialization
        elif self.game_state == "title":
            self.render_title_screen()
            return
        
        if self.show_inventory:
            self.render_inventory()
            return
            
        current_time = time.time()
        
        # Check for petal respawns
        slots_to_respawn = []
        for slot, (item, respawn_time) in self.broken_petals.items():
            if current_time >= respawn_time:
                # Create a fresh copy of the item with full health
                new_petal = Item(item.name, item.color, item.damage, 
                               item.radius, item.max_health)
                new_petal.reset_health()  # Ensure full health
                self.players[self.my_id]['equipped_petals'][slot] = new_petal
                slots_to_respawn.append(slot)
        
        # Remove respawned petals from broken_petals
        for slot in slots_to_respawn:
            del self.broken_petals[slot]
        
        # Create a surface for the world
        world_surface = pygame.Surface((self.world_width, self.world_height))
        
        # Draw zones
        for zone_name, zone_data in self.zones.items():
            pygame.draw.rect(world_surface, zone_data['color'], zone_data['rect'])
        
        # Draw zone borders
        for zone_data in self.zones.values():
            pygame.draw.rect(world_surface, (100, 100, 100), zone_data['rect'], 2)

        # Draw grid
        grid_size = 50
        for x in range(0, self.world_width, grid_size):
            pygame.draw.line(world_surface, (33, 159, 91, 128), (x, 0), (x, self.world_height))
        for y in range(0, self.world_height, grid_size):
            pygame.draw.line(world_surface, (33, 159, 91, 128), (0, y), (self.world_width, y))
        
        # Render dropped items
        for dropped_item in self.dropped_items:
            dropped_item.update()
            dropped_item.render(world_surface)
        
        # Check for item pickup
        if self.my_id in self.players:
            player = self.players[self.my_id]
            items_to_remove = []
            
            for dropped_item in self.dropped_items:
                distance = math.sqrt((player['x'] - dropped_item.x)**2 + 
                                   (player['y'] - dropped_item.y)**2)
                if distance < 30:  # Pickup radius
                    self.add_item_to_inventory(player, dropped_item.item)
                    items_to_remove.append(dropped_item)
                    print(f"Picked up: {dropped_item.item.name}")
            
            # Remove collected items
            for item in items_to_remove:
                self.dropped_items.remove(item)
        
        for player_id, player in self.players.items():
            # Draw player
            world_surface.blit(
                player['image'],
                (player['x'] - self.player_image.get_width() // 2, 
                 player['y'] - self.player_image.get_height() // 2)
            )
            
            # Draw player health bar
            self.draw_health_bar(world_surface, player['x'], player['y'] - 30, 
                               player['health'], 100)
            
            # Update and draw petals
            player['angle'] += self.orbit_speed
            for i, petal in enumerate(player['equipped_petals']):
                if petal is None:  # Skip if petal is broken
                    continue
                
                angle = player['angle'] + (2 * math.pi / self.petal_count) * i
                petal_x = player['x'] + self.orbit_radius * math.cos(angle)
                petal_y = player['y'] + self.orbit_radius * math.sin(angle)
                
                # Draw petal
                pygame.draw.circle(world_surface, petal.color, 
                                 (int(petal_x), int(petal_y)), petal.radius)
                
                # Check collision with monsters
                monsters_to_remove = []
                for monster in self.monsters:
                    if self.check_collision(petal_x, petal_y, monster, petal=petal):
                        # Petal takes damage when hitting monsters
                        if petal.take_damage(5):  # Petal damage amount
                            # Store broken petal info and schedule respawn
                            self.broken_petals[i] = (
                                Item(petal.name, petal.color, petal.damage, 
                                     petal.radius, petal.max_health),
                                current_time + self.petal_respawn_time
                            )
                            player['equipped_petals'][i] = None  # Remove broken petal
                            break
                        
                        # Calculate bounce vector for monster
                        if not hasattr(monster, 'is_static') or not monster.is_static:
                            dx = monster.x - petal_x
                            dy = monster.y - petal_y
                            distance = math.sqrt(dx * dx + dy * dy)
                            if distance > 0:
                                # Normalize and apply bounce force
                                bounce_strength = 30
                                dx = (dx / distance) * bounce_strength
                                dy = (dy / distance) * bounce_strength
                                monster.x += dx
                                monster.y += dy
                                
                                # Add some randomness to prevent predictable bouncing
                                monster.x += random.uniform(-5, 5)
                                monster.y += random.uniform(-5, 5)
                        
                        if monster.take_damage(petal.damage):
                            monsters_to_remove.append(monster)
                            # Drop a rock item if the monster is a Rock
                            if isinstance(monster, Rock):
                                self.dropped_items.append(DroppedItem(RockItem(), monster.x, monster.y))
                            else:
                                dropped_item = random.choice(self.possible_items)
                                self.dropped_items.append(DroppedItem(dropped_item, monster.x, monster.y))
                
                # Check collision with player center (flower)
                for monster in self.monsters:
                    if not hasattr(monster, 'is_static') or not monster.is_static:
                        dx = monster.x - player['x']
                        dy = monster.y - player['y']
                        distance = math.sqrt(dx * dx + dy * dy)
                        min_distance = self.player_image.get_width() // 2 + monster.radius
                        
                        if distance < min_distance:
                            # Calculate bounce vector from flower center
                            if distance > 0:
                                bounce_strength = 40  # Stronger bounce from center
                                dx = (dx / distance) * bounce_strength
                                dy = (dy / distance) * bounce_strength
                                monster.x = player['x'] + dx
                                monster.y = player['y'] + dy
                                
                                # Add slight randomness to prevent perfect bouncing
                                monster.x += random.uniform(-3, 3)
                                monster.y += random.uniform(-3, 3)
                            
                            # Damage the player
                            player['health'] -= monster.damage
                            if player['health'] <= 0:
                                self.show_death_screen()
                                return
                
                # Remove dead monsters and add new ones
                for monster in monsters_to_remove:
                    self.monsters.remove(monster)
                    if hasattr(monster, 'is_static') and monster.is_static:
                        # Create a new static monster of the same type
                        new_monster = type(monster)(
                            random.randint(50, self.world_width - 50),
                            random.randint(50, self.world_height - 50)
                        )
                        self.monsters.append(new_monster)
                    else:
                        # Create a new mobile monster
                        self.monsters.append(self.create_mobile_monster())
        
        # Update and render monsters
        for i, monster in enumerate(self.monsters):
            # Apply collision avoidance between monsters
            if not hasattr(monster, 'is_static') or not monster.is_static:
                separation = pygame.math.Vector2(0, 0)
                nearby_count = 0
                
                # Check collision with other monsters
                for other in self.monsters:
                    if monster != other:
                        distance = math.sqrt((monster.x - other.x)**2 + (monster.y - other.y)**2)
                        min_distance = monster.radius + other.radius + 20  # Add some buffer space
                        
                        if distance < min_distance:
                            # Calculate separation vector
                            if distance > 0:  # Avoid division by zero
                                diff = pygame.math.Vector2(monster.x - other.x, monster.y - other.y)
                                diff = diff.normalize() * (min_distance - distance)
                                separation += diff
                                nearby_count += 1
                
                # Apply separation if there are nearby monsters
                if nearby_count > 0:
                    separation = separation / nearby_count
                    # Apply the separation more strongly to smaller monsters
                    separation_strength = 1.0
                    if isinstance(monster, Mouse):
                        separation_strength = 1.5
                    elif isinstance(monster, Cat):
                        separation_strength = 1.2
                    
                    monster.x += separation.x * separation_strength
                    monster.y += separation.y * separation_strength
                
                # Keep monsters within world bounds
                monster.x = max(monster.radius, min(self.world_width - monster.radius, monster.x))
                monster.y = max(monster.radius, min(self.world_height - monster.radius, monster.y))
            
            monster.update(self.players)  # Update monster AI
            monster.render(world_surface)
            
            # Check collision with player
            if self.my_id in self.players:
                player = self.players[self.my_id]
                if self.check_collision(player['x'], player['y'], monster, player_collision=True):
                    player['health'] -= monster.damage
                    if player['health'] <= 0:
                        self.show_death_screen()
                        return
        
        # Determine camera position
        player = self.players[self.my_id]
        camera_x = max(0, min(player['x'] - 400, self.world_width - 800))
        camera_y = max(0, min(player['y'] - 300, self.world_height - 600))
        
        # Blit the world surface onto the screen
        self.screen.blit(world_surface, (0, 0), (camera_x, camera_y, 800, 600))
        
        # After blitting the world surface, draw the UI overlay
        self.render_ui_overlay()
        
        pygame.display.flip()
        self.clock.tick(60)
    
    def draw_health_bar(self, surface, x, y, current_health, max_health):
        bar_length = 40
        bar_height = 5
        fill = (current_health / max_health) * bar_length
        outline_rect = pygame.Rect(x - bar_length // 2, y, bar_length, bar_height)
        fill_rect = pygame.Rect(x - bar_length // 2, y, fill, bar_height)
        pygame.draw.rect(surface, (255, 0, 0), fill_rect)
        pygame.draw.rect(surface, (255, 255, 255), outline_rect, 1)
    
    def show_death_screen(self):
        # Drop three random petals from the player's equipped petals
        player = self.players[self.my_id]
        dropped_petals = 0
        for i in range(self.petal_count):
            if player['equipped_petals'][i] is not None and dropped_petals < 3:
                # Drop the petal
                dropped_item = DroppedItem(player['equipped_petals'][i], player['x'], player['y'])
                self.dropped_items.append(dropped_item)
                player['equipped_petals'][i] = None
                dropped_petals += 1

        # Create a transparent overlay
        overlay = pygame.Surface((800, 600), pygame.SRCALPHA)
        overlay.fill((0, 0, 0, 128))  # Semi-transparent black

        # Create an opaque button
        button_text = "Continue"
        font = pygame.font.Font(None, 74)
        button_surface = pygame.Surface((200, 100))
        button_surface.fill((0, 128, 0))  # Opaque green
        text = font.render(button_text, True, (255, 255, 255))
        text_rect = text.get_rect(center=(100, 50))
        button_surface.blit(text, text_rect)
        button_rect = button_surface.get_rect(center=(400, 300))

        # Display the overlay and button
        self.screen.blit(overlay, (0, 0))
        self.screen.blit(button_surface, button_rect)
        pygame.display.flip()

        # Wait for the player to click the button
        while True:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    pygame.quit()
                    sys.exit()
                elif event.type == pygame.MOUSEBUTTONDOWN:
                    if button_rect.collidepoint(event.pos):
                        self.reset_game()
                        return

    def reset_game(self):
        # Reset player health and position to the easy zone
        player = self.players[self.my_id]
        player['health'] = self.max_health  # Use max_health instead of hardcoded 100
        
        # Reset heal timer
        self.last_heal_time = time.time()

        # Respawn in the easy zone
        easy_zone = self.zones['easy']['rect']
        player['x'] = random.randint(easy_zone.left + 50, easy_zone.right - 50)
        player['y'] = random.randint(easy_zone.top + 50, easy_zone.bottom - 50)

        # Optionally reset other game elements as needed
    
    def check_collision(self, x1, y1, monster, player_collision=False, petal=None):
        distance = math.sqrt((x1 - monster.x) ** 2 + (y1 - monster.y) ** 2)
        if player_collision:
            return distance < self.player_image.get_width() // 2 + monster.radius
        return distance < petal.radius + monster.radius

    def handle_inventory_click(self, mouse_pos):
        if self.my_id not in self.players:
            return
        
        player = self.players[self.my_id]
        inventory_pos = (mouse_pos[0] - 100, mouse_pos[1] - 100)
        
        # Check clicks on inventory items
        i = 0
        for item_name, item_data in player['inventory'].items():
            pos_x = 50 + (i % 8) * 60
            pos_y = 170 + (i // 8) * 60
            distance = math.sqrt((inventory_pos[0] - pos_x)**2 + (inventory_pos[1] - pos_y)**2)
            
            if distance < 20:
                slot = self.show_slot_selection()
                if slot is not None:
                    # Swap items
                    old_petal = player['equipped_petals'][slot]
                    player['equipped_petals'][slot] = item_data['item']
                    
                    # Add old petal back to inventory only if it exists
                    if old_petal is not None:
                        self.add_item_to_inventory(player, old_petal)
                    
                    # Decrease count of equipped item
                    item_data['count'] -= 1
                    if item_data['count'] <= 0:
                        del player['inventory'][item_name]
                    
                    self.show_inventory = False
                    return
            i += 1

    def render_ui_overlay(self):
        # Create UI bar at the bottom
        ui_height = 80
        ui_surface = pygame.Surface((800, ui_height))
        ui_surface.fill((99, 255, 133))  # Dark gray background
        
        # Draw inventory button
        button_size = 60
        inventory_button = pygame.Rect(10, 10, button_size, button_size)
        pygame.draw.rect(ui_surface, (99, 120, 255), inventory_button)
        
        # Draw "I" text on button
        font = pygame.font.Font(None, 36)
        text = font.render("I", True, (255, 255, 255))
        text_rect = text.get_rect(center=inventory_button.center)
        ui_surface.blit(text, text_rect)
        
        # Draw equipped petals
        if self.my_id in self.players:
            player = self.players[self.my_id]
            petal_size = 40
            start_x = 100
            
            current_time = time.time()
            
            for i in range(self.petal_count):
                # Draw slot background
                slot_rect = pygame.Rect(start_x + i * (petal_size + 10), 20, petal_size, petal_size)
                pygame.draw.rect(ui_surface, (30, 30, 30), slot_rect)
                
                petal = player['equipped_petals'][i]
                if petal is not None:
                    # Draw base petal
                    pygame.draw.circle(ui_surface, petal.color, 
                                     (start_x + i * (petal_size + 10) + petal_size // 2, 
                                      20 + petal_size // 2), 
                                     petal_size // 2 - 2)
                    
                    # Draw gray overlay based on health
                    if petal.health < petal.max_health:
                        health_ratio = 1 - (petal.health / petal.max_health)
                        overlay_height = int(petal_size * health_ratio)
                        if overlay_height > 0:
                            overlay_rect = pygame.Rect(
                                start_x + i * (petal_size + 10),
                                20,
                                petal_size,
                                overlay_height
                            )
                            overlay_surface = pygame.Surface((petal_size, overlay_height))
                            overlay_surface.fill((128, 128, 128))
                            overlay_surface.set_alpha(180)
                            ui_surface.blit(overlay_surface, overlay_rect)
                
                elif i in self.broken_petals:
                    # Draw respawn timer for broken petals
                    _, respawn_time = self.broken_petals[i]
                    remaining = max(0, respawn_time - current_time)
                    timer_text = font.render(f"{remaining:.1f}", True, (200, 200, 200))
                    timer_rect = timer_text.get_rect(center=(
                        start_x + i * (petal_size + 10) + petal_size // 2,
                        20 + petal_size // 2
                    ))
                    ui_surface.blit(timer_text, timer_rect)
                else:
                    # Draw X for empty slots
                    broken_color = (100, 100, 100)
                    center_x = start_x + i * (petal_size + 10) + petal_size // 2
                    center_y = 20 + petal_size // 2
                    pygame.draw.line(ui_surface, broken_color, 
                                   (center_x - 10, center_y - 10),
                                   (center_x + 10, center_y + 10), 2)
                    pygame.draw.line(ui_surface, broken_color,
                                   (center_x + 10, center_y - 10),
                                   (center_x - 10, center_y + 10), 2)
                
                # Draw slot number
                slot_num = font.render(str(i + 1), True, (200, 200, 200))
                num_rect = slot_num.get_rect(bottomright=(
                    start_x + i * (petal_size + 10) + petal_size - 2, 
                    20 + petal_size - 2
                ))
                ui_surface.blit(slot_num, num_rect)
        
        # Blit UI surface at the bottom of the screen
        self.screen.blit(ui_surface, (0, 520))  # 520 = 600 - ui_height

    def load_svg_image(self, svg_path, size=None, dpi=96):
        """Load SVG with optional size parameter and DPI"""
        try:
            # Read SVG file
            with open(svg_path, 'rb') as f:
                svg_data = f.read()
            
            # Convert SVG to PNG in memory
            png_data = svg2png(svg_data, dpi=dpi)
            
            # Convert to PIL Image
            image = Image.open(BytesIO(png_data))
            
            # Resize if size is specified
            if size:
                image = image.resize(size, Image.Resampling.LANCZOS)
            
            # Convert to pygame surface
            return pygame.image.fromstring(
                image.tobytes(), image.size, image.mode).convert_alpha()
        except Exception as e:
            print(f"Error loading image {svg_path}: {e}")
            return None

    def render_title_screen(self):
        # Fill background
        self.screen.fill((28, 168, 99))  # Light green background
        
        # Create fonts
        font_title = pygame.font.Font(None, 74)  # Smaller title font
        font_button = pygame.font.Font(None, 48)  # Button font
        font_small = pygame.font.Font(None, 24)  # Smaller controls font
        
        # Create title text
        title_text = font_title.render("florr.io", True, (255, 255, 255))
        title_rect = title_text.get_rect(center=(400, 100))
        
        # Create IP input box
        input_box = pygame.Rect(300, 350, 200, 32)
        input_color = (255, 255, 255) if self.input_active else (200, 200, 200)
        pygame.draw.rect(self.screen, input_color, input_box, 2)
        
        # Render input text
        input_surface = font_small.render(self.input_text, True, (255, 255, 255))
        input_rect = input_surface.get_rect(center=input_box.center)
        self.screen.blit(input_surface, input_rect)
        
        # Add input box label
        label = font_small.render("Server IP:Port (optional)", True, (255, 255, 255))
        label_rect = label.get_rect(bottom=input_box.top - 5, centerx=input_box.centerx)
        self.screen.blit(label, label_rect)
        
        # Create start button
        button_surface = pygame.Surface((200, 60))
        button_surface.fill((0, 128, 0))  # Green button
        pygame.draw.rect(button_surface, (255, 255, 255), button_surface.get_rect(), 2)
        
        button_text = font_button.render("Ready", True, (255, 255, 255))
        button_text_rect = button_text.get_rect(center=(100, 30))
        button_surface.blit(button_text, button_text_rect)
        
        button_rect = button_surface.get_rect(center=(400, 450))
        
        # Controls text
        controls_text = [
            "Controls:",
            "WASD / Arrow Keys - Move",
            "Space / Left Click - Attack",
            "Shift / Right Click - Defend",
        ]
        
        # Draw everything
        self.screen.blit(title_text, title_rect)
        self.screen.blit(button_surface, button_rect)
        
        # Draw controls text
        for i, text in enumerate(controls_text):
            control_text = font_small.render(text, True, (255, 255, 255))
            control_rect = control_text.get_rect(center=(400, 200 + i * 25))
            self.screen.blit(control_text, control_rect)
        
        # Store rects for click/input detection
        self.start_button_rect = button_rect
        self.input_box_rect = input_box
        
        pygame.display.flip()

    def start_game(self):
        """Handle transition from title screen to game"""
        self.game_state = "loading"
        self.loading_progress = 0
        
        # Initialize game world
        self.render_loading_screen("Initializing game world...")
        self.loading_progress = 20
        pygame.time.wait(100)  # Small delay to show progress
        
        # Initialize monsters
        self.render_loading_screen("Spawning monsters...")
        self.loading_progress = 40
        pygame.time.wait(100)
        
        # Load player data
        self.render_loading_screen("Loading player data...")
        self.loading_progress = 60
        pygame.time.wait(100)
        
        # Fill empty petal slots with basic petals
        if self.my_id in self.players:
            player = self.players[self.my_id]
            for i in range(self.petal_count):
                if player['equipped_petals'][i] is None:
                    basic_petal = Item("Basic Petal", (255, 255, 255), damage=10, radius=10, max_health=100)
                    player['equipped_petals'][i] = basic_petal
        
        pygame.time.wait(100)
        
        # Initialize network
        self.render_loading_screen("Connecting to network...")
        self.loading_progress = 80
        pygame.time.wait(100)
        
        # Final setup
        self.render_loading_screen("Finalizing...")
        self.loading_progress = 100
        pygame.time.wait(500)  # Show 100% for half a second
        
        # Switch to playing state
        self.game_state = "playing"