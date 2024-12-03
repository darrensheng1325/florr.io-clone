import pygame
import json
import math
import random

class GameEngine:
    def __init__(self, width=800, height=600):
        pygame.init()
        self.screen = pygame.display.set_mode((width, height))
        pygame.display.set_caption("P2P Game")
        self.clock = pygame.time.Clock()
        
        # Load player image
        self.player_image = pygame.image.load("player.png")
        self.player_image = pygame.transform.scale(self.player_image, (40, 40))  # Scale image to desired size
        
        # Petal properties
        self.petal_count = 5
        self.petal_radius = 10
        self.orbit_radius = 50
        self.orbit_speed = 0.05
        
        self.players = {}  # Dictionary to store all players' positions
        self.my_id = None
        
        # Monster properties
        self.monsters = [Monster(random.randint(50, width-50), random.randint(50, height-50)) for _ in range(5)]
        
    def add_player(self, player_id, x=400, y=300):
        self.players[player_id] = {
            'x': x,
            'y': y,
            'image': self.player_image,
            'angle': 0  # Initial angle for petal orbit
        }
    
    def update_player(self, player_id, x, y):
        if player_id in self.players:
            self.players[player_id]['x'] = x
            self.players[player_id]['y'] = y
    
    def handle_local_input(self):
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
    
    def render(self):
        self.screen.fill((0, 128, 0))  # Green background
        
        for player_id, player in self.players.items():
            # Draw player
            self.screen.blit(
                player['image'],
                (player['x'] - self.player_image.get_width() // 2, player['y'] - self.player_image.get_height() // 2)
            )
            
            # Update and draw petals
            player['angle'] += self.orbit_speed
            for i in range(self.petal_count):
                angle = player['angle'] + (2 * math.pi / self.petal_count) * i
                petal_x = player['x'] + self.orbit_radius * math.cos(angle)
                petal_y = player['y'] + self.orbit_radius * math.sin(angle)
                pygame.draw.circle(self.screen, (255, 255, 255), (int(petal_x), int(petal_y)), self.petal_radius)  # White petals
                
                # Check collision with monsters
                for monster in self.monsters:
                    if self.check_collision(petal_x, petal_y, monster):
                        if monster.take_damage(10):  # Damage amount
                            self.monsters.remove(monster)
            
        # Render monsters
        for monster in self.monsters:
            monster.render(self.screen)
            
        pygame.display.flip()
        self.clock.tick(60)
    
    def check_collision(self, petal_x, petal_y, monster):
        distance = math.sqrt((petal_x - monster.x) ** 2 + (petal_y - monster.y) ** 2)
        return distance < self.petal_radius + monster.radius

class Monster:
    def __init__(self, x, y, health=100):
        self.x = x
        self.y = y
        self.health = health
        self.radius = 20  # Size of the monster
        self.color = (0, 0, 255)  # Blue color for monsters

    def take_damage(self, amount):
        self.health -= amount
        if self.health <= 0:
            return True  # Monster is dead
        return False

    def render(self, screen):
        pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)