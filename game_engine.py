import pygame
import json
import math
import random
import sys
import time

class Item:
    def __init__(self, name, color, damage=10, radius=10, max_health=100):
        self.name = name
        self.color = color
        self.damage = damage
        self.radius = radius
        self.max_health = max_health
        self.health = max_health  # Current health
        
    def __eq__(self, other):
        return isinstance(other, Item) and self.name == other.name
    
    def take_damage(self, amount):
        self.health -= amount
        if self.health <= 0:
            return True  # Petal is broken
        return False
    
    def reset_health(self):
        self.health = self.max_health

class DroppedItem:
    def __init__(self, item, x, y):
        self.item = item
        self.x = x
        self.y = y
        self.radius = 15
        self.bob_offset = 0
        self.bob_speed = 0.1

    def update(self):
        # Make the item bob up and down
        self.bob_offset = math.sin(pygame.time.get_ticks() * self.bob_speed) * 5

    def render(self, surface):
        # Draw the item with a slight glow effect
        glow_radius = self.radius + 5
        pygame.draw.circle(surface, (255, 255, 255, 128), 
                         (int(self.x), int(self.y + self.bob_offset)), 
                         glow_radius)
        pygame.draw.circle(surface, self.item.color, 
                         (int(self.x), int(self.y + self.bob_offset)), 
                         self.radius)

class GameEngine:
    def __init__(self, width=4800, height=1200, initial_monster_count=5):
        pygame.init()
        self.screen = pygame.display.set_mode((800, 600))
        pygame.display.set_caption("P2P Game")
        self.clock = pygame.time.Clock()
        
        # World dimensions
        self.world_width = width
        self.world_height = height
        
        # Load player image
        self.player_image = pygame.image.load("player.png")
        self.player_image = pygame.transform.scale(self.player_image, (40, 40))
        
        # Define possible items with different health values
        self.possible_items = [
            Item("Basic Petal", (255, 255, 255), damage=10, radius=10, max_health=100),
            Item("Fire Petal", (255, 100, 0), damage=15, radius=12, max_health=80),
            Item("Ice Petal", (0, 255, 255), damage=12, radius=11, max_health=120),
            Item("Poison Petal", (0, 255, 0), damage=8, radius=13, max_health=150)
        ]
        
        # Petal properties
        self.petal_count = 5
        self.orbit_radius = 50
        self.orbit_speed = 0.05
        
        self.players = {}
        self.my_id = None
        
        # Define monster types and their spawn weights
        self.monster_types = {
            Mouse: 30,    # 30% chance for mouse
            Cat: 20,      # 20% chance for cat
            Tank: 10,     # 10% chance for tank
            Bush: 20,     # 20% chance for bush
            Tree: 10,     # 10% chance for tree
            Rock: 10      # 10% chance for rock
        }
        
        # Add zone definitions (side by side, each 1600 pixels wide)
        self.zones = {
            'easy': {
                'color': (144, 238, 144),  # Light green
                'rect': pygame.Rect(0, 0, width // 3, height),  # Now 1600 pixels wide
                'monster_multiplier': 1.0,  # Base difficulty
                'spawn_weights': {
                    Mouse: 50,    # More mice in easy zone
                    Cat: 10,
                    Tank: 5,
                    Bush: 20,
                    Tree: 10,
                    Rock: 5
                }
            },
            'medium': {
                'color': (255, 218, 185),  # Peach
                'rect': pygame.Rect(width // 3, 0, width // 3, height),  # Now 1600 pixels wide
                'monster_multiplier': 1.5,  # 50% stronger
                'spawn_weights': {
                    Mouse: 30,
                    Cat: 30,
                    Tank: 15,
                    Bush: 10,
                    Tree: 10,
                    Rock: 5
                }
            },
            'hard': {
                'color': (255, 160, 122),  # Salmon
                'rect': pygame.Rect(2 * width // 3, 0, width // 3, height),  # Now 1600 pixels wide
                'monster_multiplier': 2.0,  # Double strength
                'spawn_weights': {
                    Mouse: 20,
                    Cat: 35,
                    Tank: 25,
                    Bush: 5,
                    Tree: 10,
                    Rock: 5
                }
            }
        }
        
        # Increase initial monster count to account for larger map
        initial_monster_count *= 3  # Triple the monsters for triple the width
        
        # Create initial monsters
        self.monsters = []
        # First add static monsters in good positions
        self.add_static_monsters(initial_monster_count * 2)  # More static monsters
        # Then add mobile monsters
        for _ in range(initial_monster_count):
            self.monsters.append(self.create_mobile_monster())
        
        # Show inventory flag
        self.show_inventory = False
        
        # Add dropped items list
        self.dropped_items = []
        
        # Load monster images
        self.mouse_image = pygame.image.load("mouse.svg")
        self.mouse_image = pygame.transform.scale(self.mouse_image, (30, 30))
        self.cat_image = pygame.image.load("cat.svg")
        self.cat_image = pygame.transform.scale(self.cat_image, (50, 50))
        
        # Pass images to monster classes
        Mouse.image = self.mouse_image
        Cat.image = self.cat_image
        
        # Add broken petal tracking
        self.broken_petals = {}  # {slot_index: (item, respawn_time)}
        self.petal_respawn_time = 2  # Seconds until respawn
    
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
        
        # Choose monster type based on zone weights
        weights = list(zone_data['spawn_weights'].values())
        monster_type = random.choices(
            list(zone_data['spawn_weights'].keys()),
            weights=weights
        )[0]
        
        # Create monster and adjust its stats based on zone
        monster = monster_type(x, y)
        multiplier = zone_data['monster_multiplier']
        monster.health *= multiplier
        monster.damage *= multiplier
        
        return monster

    def get_zone(self, x, y):
        for zone_name, zone_data in self.zones.items():
            if zone_data['rect'].collidepoint(x, y):
                return zone_name
        return 'easy'  # Default to easy zone
    
    def add_player(self, player_id, x=800, y=600):
        # When adding items to equipped_petals, create new instances
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
    
    def update_player(self, player_id, x, y):
        if player_id in self.players:
            self.players[player_id]['x'] = x
            self.players[player_id]['y'] = y
    
    def handle_local_input(self):
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
        # Create a new instance of the item to ensure separate health values
        new_item = Item(item.name, item.color, item.damage, item.radius, item.max_health)
        if item.name in player['inventory']:
            player['inventory'][item.name]['count'] += 1
        else:
            player['inventory'][item.name] = {'item': new_item, 'count': 1}
    
    def render_inventory(self):
        inventory_surface = pygame.Surface((600, 400))
        inventory_surface.fill((50, 50, 50))
        
        player = self.players[self.my_id]
        font = pygame.font.Font(None, 36)
        small_font = pygame.font.Font(None, 24)
        
        # Draw equipped petals
        text = font.render("Equipped Petals:", True, (255, 255, 255))
        inventory_surface.blit(text, (20, 20))
        
        for i, petal in enumerate(player['equipped_petals']):
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
            pygame.draw.line(world_surface, (200, 200, 200, 128), (x, 0), (x, self.world_height))
        for y in range(0, self.world_height, grid_size):
            pygame.draw.line(world_surface, (200, 200, 200, 128), (0, y), (self.world_width, y))
        
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
                (player['x'] - self.player_image.get_width() // 2, player['y'] - self.player_image.get_height() // 2)
            )
            
            # Draw player health bar
            self.draw_health_bar(world_surface, player['x'], player['y'] - 30, player['health'], 100)
            
            # Update and draw petals
            player['angle'] += self.orbit_speed
            for i, petal in enumerate(player['equipped_petals']):
                if petal is None:  # Skip if petal is broken
                    continue
                
                angle = player['angle'] + (2 * math.pi / self.petal_count) * i
                petal_x = player['x'] + self.orbit_radius * math.cos(angle)
                petal_y = player['y'] + self.orbit_radius * math.sin(angle)
                
                # Draw petal without health indicator
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
                        
                        # Monster takes damage and knockback
                        if not hasattr(monster, 'is_static') or not monster.is_static:
                            dx = monster.x - player['x']
                            dy = monster.y - player['y']
                            distance = math.sqrt(dx * dx + dy * dy)
                            if distance > 0:
                                knockback_strength = 20
                                dx = (dx / distance) * knockback_strength
                                dy = (dy / distance) * knockback_strength
                                monster.x += dx
                                monster.y += dy
                        
                        if monster.take_damage(petal.damage):
                            monsters_to_remove.append(monster)
                            dropped_item = random.choice(self.possible_items)
                            self.dropped_items.append(DroppedItem(dropped_item, monster.x, monster.y))
                
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
        for monster in self.monsters:
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
        start_time = time.time()
        font = pygame.font.Font(None, 74)
        small_font = pygame.font.Font(None, 36)
        text = font.render('You Died', True, (255, 0, 0))
        text_rect = text.get_rect(center=(400, 200))
        
        button_text = small_font.render('Continue', True, (255, 255, 255))
        button_rect = button_text.get_rect(center=(400, 400))
        
        while True:
            self.screen.fill((0, 0, 0))
            self.screen.blit(text, text_rect)
            
            # Draw continue button (always visible)
            pygame.draw.rect(self.screen, (0, 128, 0), button_rect.inflate(20, 10))
            self.screen.blit(button_text, button_rect)
            
            # Calculate and display remaining time
            elapsed_time = time.time() - start_time
            remaining_time = max(0, 10 - int(elapsed_time))
            timer_text = small_font.render(f'Respawn in: {remaining_time}s', True, (255, 255, 255))
            timer_rect = timer_text.get_rect(center=(400, 300))
            self.screen.blit(timer_text, timer_rect)
            
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    pygame.quit()
                    sys.exit()
                elif event.type == pygame.MOUSEBUTTONDOWN:
                    if button_rect.collidepoint(event.pos) and elapsed_time >= 10:
                        self.reset_game()
                        return
            
            pygame.display.flip()
            self.clock.tick(60)
    
    def reset_game(self):
        # Reset player health and position
        player = self.players[self.my_id]
        player['health'] = 100
        player['x'] = 800
        player['y'] = 600
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
                    player['equipped_petals'][slot] = item_data['item']  # Use the item object from item_data
                    
                    # Add old petal back to inventory
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
        ui_surface.fill((50, 50, 50))  # Dark gray background
        
        # Draw inventory button
        button_size = 60
        inventory_button = pygame.Rect(10, 10, button_size, button_size)
        pygame.draw.rect(ui_surface, (70, 70, 70), inventory_button)
        
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

class Monster:
    def __init__(self, x, y, health=100):
        self.x = x
        self.y = y
        self.health = health
        self.radius = 20
        self.color = (0, 0, 255)
        self.speed = 2
        self.target_x = x
        self.target_y = y
        self.update_delay = 0
        self.knockback_resistance = 0.95
        self.velocity_x = 0
        self.velocity_y = 0
        self.damage = 1
        self.name = "Basic Monster"
    
    def find_nearest_player(self, players):
        nearest_distance = float('inf')
        nearest_x = self.x
        nearest_y = self.y
        
        for player in players.values():
            dx = player['x'] - self.x
            dy = player['y'] - self.y
            distance = math.sqrt(dx * dx + dy * dy)
            
            if distance < nearest_distance:
                nearest_distance = distance
                nearest_x = player['x']
                nearest_y = player['y']
        
        return nearest_x, nearest_y

    def update(self, players):
        # Apply velocity (for knockback)
        self.x += self.velocity_x
        self.y += self.velocity_y
        
        # Reduce velocity (friction)
        self.velocity_x *= self.knockback_resistance
        self.velocity_y *= self.knockback_resistance
        
        # Normal movement
        self.update_delay += 1
        if self.update_delay >= 30:
            self.target_x, self.target_y = self.find_nearest_player(players)
            self.update_delay = 0
        
        # Only move if knockback is small
        if abs(self.velocity_x) < 0.5 and abs(self.velocity_y) < 0.5:
            dx = self.target_x - self.x
            dy = self.target_y - self.y
            distance = math.sqrt(dx * dx + dy * dy)
            
            if distance > 0:
                dx = dx / distance + random.uniform(-0.2, 0.2)
                dy = dy / distance + random.uniform(-0.2, 0.2)
                
                self.x += dx * self.speed
                self.y += dy * self.speed

    def take_damage(self, amount):
        self.health -= amount
        if self.health <= 0:
            return True  # Monster is dead
        return False

    def get_angle_to_target(self):
        # Calculate angle to target for rotation
        dx = self.target_x - self.x
        dy = self.target_y - self.y
        return math.degrees(math.atan2(dy, dx))

    def render(self, screen):
        if hasattr(self, 'image') and self.image:
            # Rotate image to face target
            angle = self.get_angle_to_target()
            rotated_image = pygame.transform.rotate(self.image, -angle - 90)  # -90 to adjust for image orientation
            screen.blit(rotated_image, 
                       (int(self.x - rotated_image.get_width()/2), 
                        int(self.y - rotated_image.get_height()/2)))
        else:
            # Fallback to circle if no image
            pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        
        # Draw health bar
        self.draw_health_bar(screen, self.x, self.y - 30, self.health, 100)

    def draw_health_bar(self, surface, x, y, current_health, max_health):
        bar_length = 40
        bar_height = 5
        fill = (current_health / max_health) * bar_length
        outline_rect = pygame.Rect(x - bar_length // 2, y, bar_length, bar_height)
        fill_rect = pygame.Rect(x - bar_length // 2, y, fill, bar_height)
        pygame.draw.rect(surface, (255, 0, 0), fill_rect)
        pygame.draw.rect(surface, (255, 255, 255), outline_rect, 1)

class Mouse(Monster):
    image = None  # Will be set by GameEngine
    
    def __init__(self, x, y):
        super().__init__(x, y, health=50)
        self.color = (150, 150, 150)
        self.speed = 3
        self.radius = 15
        self.knockback_resistance = 0.98
        self.damage = 0.5
        self.name = "Mouse"
    
    def render(self, screen):
        if self.image:
            angle = self.get_angle_to_target()
            rotated_image = pygame.transform.rotate(self.image, -angle - 90)
            screen.blit(rotated_image, 
                       (int(self.x - rotated_image.get_width()/2), 
                        int(self.y - rotated_image.get_height()/2)))
        else:
            super().render(screen)
        self.draw_health_bar(screen, self.x, self.y - 30, self.health, 50)

class Cat(Monster):
    image = None  # Will be set by GameEngine
    
    def __init__(self, x, y):
        super().__init__(x, y, health=150)
        self.color = (255, 140, 0)
        self.speed = 2.5
        self.radius = 25
        self.knockback_resistance = 0.9
        self.damage = 2
        self.name = "Cat"
        self.dash_cooldown = 0
        self.is_dashing = False
    
    def render(self, screen):
        if self.image:
            angle = self.get_angle_to_target()
            rotated_image = pygame.transform.rotate(self.image, -angle + 90)
            screen.blit(rotated_image, 
                       (int(self.x - rotated_image.get_width()/2), 
                        int(self.y - rotated_image.get_height()/2)))
        else:
            super().render(screen)
        self.draw_health_bar(screen, self.x, self.y - 40, self.health, 150)

class Tank(Monster):
    def __init__(self, x, y):
        super().__init__(x, y, health=300)
        self.color = (139, 69, 19)
        self.speed = 1
        self.radius = 30
        self.knockback_resistance = 0.8
        self.damage = 3
        self.name = "Tank"

class StaticMonster(Monster):
    def __init__(self, x, y, health=100):
        super().__init__(x, y, health)
        self.is_static = True
        self.damage = 0  # Static monsters don't deal damage
    
    def update(self, players):
        # Static monsters don't move or track players
        pass

class Bush(StaticMonster):
    def __init__(self, x, y):
        super().__init__(x, y, health=500)
        self.color = (34, 139, 34)  # Forest green
        self.radius = 25
        self.name = "Bush"
        self.knockback_resistance = 1.0  # Can't be knocked back
    
    def render(self, screen):
        # Draw main bush body
        pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        # Draw darker patches for depth
        pygame.draw.circle(screen, (20, 100, 20), 
                         (int(self.x - 5), int(self.y - 5)), self.radius // 2)
        pygame.draw.circle(screen, (20, 100, 20), 
                         (int(self.x + 5), int(self.y + 5)), self.radius // 2)
        self.draw_health_bar(screen, self.x, self.y - 35, self.health, 500)

class Tree(StaticMonster):
    def __init__(self, x, y):
        super().__init__(x, y, health=1000)
        self.color = (101, 67, 33)  # Brown
        self.radius = 35
        self.name = "Tree"
        self.knockback_resistance = 1.0
    
    def render(self, screen):
        # Draw trunk
        pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius // 2)
        # Draw foliage
        pygame.draw.circle(screen, (46, 139, 87), 
                         (int(self.x), int(self.y - self.radius//2)), self.radius)
        pygame.draw.circle(screen, (46, 139, 87), 
                         (int(self.x - self.radius//2), int(self.y - self.radius//4)), self.radius)
        pygame.draw.circle(screen, (46, 139, 87), 
                         (int(self.x + self.radius//2), int(self.y - self.radius//4)), self.radius)
        self.draw_health_bar(screen, self.x, self.y - 45, self.health, 1000)

class Rock(StaticMonster):
    def __init__(self, x, y):
        super().__init__(x, y, health=2000)
        self.color = (169, 169, 169)  # Gray
        self.radius = 30
        self.name = "Rock"
        self.knockback_resistance = 1.0
    
    def render(self, screen):
        # Draw main rock body
        pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        # Draw highlights and shadows for 3D effect
        pygame.draw.circle(screen, (200, 200, 200), 
                         (int(self.x - 5), int(self.y - 5)), self.radius // 2)
        pygame.draw.circle(screen, (130, 130, 130), 
                         (int(self.x + 5), int(self.y + 5)), self.radius // 2)
        self.draw_health_bar(screen, self.x, self.y - 40, self.health, 2000)