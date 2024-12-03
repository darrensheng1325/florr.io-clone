import pygame
import json
import math
import random
import sys
import time

class Item:
    def __init__(self, name, color, damage=10, radius=10):
        self.name = name
        self.color = color
        self.damage = damage
        self.radius = radius

class GameEngine:
    def __init__(self, width=1600, height=1200, initial_monster_count=5):
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
        
        # Define possible items
        self.possible_items = [
            Item("Basic Petal", (255, 255, 255), 10, 10),
            Item("Fire Petal", (255, 100, 0), 15, 12),
            Item("Ice Petal", (0, 255, 255), 12, 11),
            Item("Poison Petal", (0, 255, 0), 8, 13)
        ]
        
        # Petal properties
        self.petal_count = 5
        self.orbit_radius = 50
        self.orbit_speed = 0.05
        
        self.players = {}
        self.my_id = None
        
        # Monster properties
        self.monsters = [self.create_monster() for _ in range(initial_monster_count)]
        self.max_monsters = initial_monster_count
        
        # Show inventory flag
        self.show_inventory = False
        
    def create_monster(self):
        x = random.randint(50, self.world_width - 50)
        y = random.randint(50, self.world_height - 50)
        return Monster(x, y)
        
    def add_player(self, player_id, x=800, y=600):
        self.players[player_id] = {
            'x': x,
            'y': y,
            'image': self.player_image,
            'angle': 0,
            'health': 100,
            'inventory': [],  # List to store collected items
            'equipped_petals': [self.possible_items[0] for _ in range(self.petal_count)]  # Start with basic petals
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
    
    def render_inventory(self):
        inventory_surface = pygame.Surface((600, 400))
        inventory_surface.fill((50, 50, 50))
        
        player = self.players[self.my_id]
        font = pygame.font.Font(None, 36)
        
        # Draw equipped petals
        text = font.render("Equipped Petals:", True, (255, 255, 255))
        inventory_surface.blit(text, (20, 20))
        
        for i, petal in enumerate(player['equipped_petals']):
            pygame.draw.circle(inventory_surface, petal.color, (50 + i * 60, 70), 20)
        
        # Draw inventory items
        text = font.render("Inventory:", True, (255, 255, 255))
        inventory_surface.blit(text, (20, 120))
        
        for i, item in enumerate(player['inventory']):
            pos_x = 50 + (i % 8) * 60
            pos_y = 170 + (i // 8) * 60
            pygame.draw.circle(inventory_surface, item.color, (pos_x, pos_y), 20)
            
            # Handle clicking on inventory items
            mouse_pos = pygame.mouse.get_pos()
            inventory_pos = (mouse_pos[0] - 100, mouse_pos[1] - 100)  # Adjust for inventory position
            
            if pygame.mouse.get_pressed()[0]:  # Left click
                distance = math.sqrt((inventory_pos[0] - pos_x)**2 + (inventory_pos[1] - pos_y)**2)
                if distance < 20:
                    # Ask which slot to equip to
                    slot = self.show_slot_selection()
                    if slot is not None:
                        player['equipped_petals'][slot] = item
                        player['inventory'].remove(item)
                        self.show_inventory = False
        
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
            
        # Create a surface for the world
        world_surface = pygame.Surface((self.world_width, self.world_height))
        world_surface.fill((0, 128, 0))  # Green background
        
        # Draw grid
        grid_size = 50
        for x in range(0, self.world_width, grid_size):
            pygame.draw.line(world_surface, (200, 200, 200), (x, 0), (x, self.world_height))
        for y in range(0, self.world_height, grid_size):
            pygame.draw.line(world_surface, (200, 200, 200), (0, y), (self.world_width, y))
        
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
                angle = player['angle'] + (2 * math.pi / self.petal_count) * i
                petal_x = player['x'] + self.orbit_radius * math.cos(angle)
                petal_y = player['y'] + self.orbit_radius * math.sin(angle)
                pygame.draw.circle(world_surface, petal.color, (int(petal_x), int(petal_y)), petal.radius)
                
                # Check collision with monsters
                monsters_to_remove = []
                for monster in self.monsters:
                    if self.check_collision(petal_x, petal_y, monster, petal=petal):
                        if monster.take_damage(petal.damage):
                            monsters_to_remove.append(monster)
                            # Drop item when monster dies
                            dropped_item = random.choice(self.possible_items)
                            if len(self.players[self.my_id]['inventory']) < 16:  # Inventory limit
                                print(f"Got new item: {dropped_item.name}")  # Debug print
                                self.players[self.my_id]['inventory'].append(dropped_item)
                
                # Remove dead monsters and add new ones
                for monster in monsters_to_remove:
                    self.monsters.remove(monster)
                    self.monsters.append(self.create_monster())
            
            # Move monsters and check collision with player
            for monster in self.monsters:
                monster.move_towards(player['x'], player['y'])
                if self.check_collision(player['x'], player['y'], monster, player_collision=True):
                    player['health'] -= 1  # Damage to player
                    if player['health'] <= 0:
                        self.show_death_screen()
                        return
        
        # Render monsters
        for monster in self.monsters:
            monster.render(world_surface)
        
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
        for i, item in enumerate(player['inventory']):
            pos_x = 50 + (i % 8) * 60
            pos_y = 170 + (i // 8) * 60
            distance = math.sqrt((inventory_pos[0] - pos_x)**2 + (inventory_pos[1] - pos_y)**2)
            if distance < 20:
                slot = self.show_slot_selection()
                if slot is not None:
                    # Swap items instead of removing
                    old_petal = player['equipped_petals'][slot]
                    player['equipped_petals'][slot] = item
                    player['inventory'][i] = old_petal
                    self.show_inventory = False
                    return

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
            start_x = 100  # Start after the inventory button
            
            # Draw slots background
            for i in range(self.petal_count):
                slot_rect = pygame.Rect(start_x + i * (petal_size + 10), 20, petal_size, petal_size)
                pygame.draw.rect(ui_surface, (30, 30, 30), slot_rect)  # Darker background for slots
                
                # Draw petal
                petal = player['equipped_petals'][i]
                pygame.draw.circle(ui_surface, petal.color, 
                                 (start_x + i * (petal_size + 10) + petal_size // 2, 
                                  20 + petal_size // 2), 
                                 petal_size // 2 - 2)
                
                # Draw slot number
                slot_num = font.render(str(i + 1), True, (200, 200, 200))
                num_rect = slot_num.get_rect(bottomright=(start_x + i * (petal_size + 10) + petal_size - 2, 
                                                          20 + petal_size - 2))
                ui_surface.blit(slot_num, num_rect)
        
        # Blit UI surface at the bottom of the screen
        self.screen.blit(ui_surface, (0, 520))  # 520 = 600 - ui_height

class Monster:
    def __init__(self, x, y, health=100):
        self.x = x
        self.y = y
        self.health = health
        self.radius = 20  # Size of the monster
        self.color = (0, 0, 255)  # Blue color for monsters
        self.speed = 2  # Speed of the monster

    def take_damage(self, amount):
        self.health -= amount
        if self.health <= 0:
            return True  # Monster is dead
        return False

    def move_towards(self, target_x, target_y):
        # Calculate direction vector
        dx = target_x - self.x
        dy = target_y - self.y
        distance = math.sqrt(dx ** 2 + dy ** 2)
        if distance > 0:
            dx /= distance
            dy /= distance
        # Move monster towards the target
        self.x += dx * self.speed
        self.y += dy * self.speed

    def render(self, screen):
        pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        # Draw monster health bar
        self.draw_health_bar(screen, self.x, self.y - 30, self.health, 100)

    def draw_health_bar(self, surface, x, y, current_health, max_health):
        bar_length = 40
        bar_height = 5
        fill = (current_health / max_health) * bar_length
        outline_rect = pygame.Rect(x - bar_length // 2, y, bar_length, bar_height)
        fill_rect = pygame.Rect(x - bar_length // 2, y, fill, bar_height)
        pygame.draw.rect(surface, (255, 0, 0), fill_rect)
        pygame.draw.rect(surface, (255, 255, 255), outline_rect, 1)