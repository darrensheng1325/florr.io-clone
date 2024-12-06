import pygame
import random
import math
import time

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
        self.preferred_zone = None
    
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
        
        # Check if monster should return to its preferred zone
        if self.preferred_zone == 'medium':
            # Get the medium zone boundaries (assuming width is 4800)
            zone_left = 1600  # Start of medium zone
            zone_right = 3200  # End of medium zone
            zone_center_x = (zone_left + zone_right) / 2
            
            # If monster is in easy zone, move back to medium
            if self.x < zone_left:
                dx = zone_center_x - self.x
                dy = 0  # Keep same Y position
                distance = abs(dx)
                if distance > 0:
                    self.x += (dx / distance) * self.speed * 2  # Move faster when returning
                return  # Skip normal movement
        
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
        self.preferred_zone = 'medium'
    
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
        self.preferred_zone = 'medium'
    
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

class Ant(Monster):
    def __init__(self, x, y):
        super().__init__(x, y)
        self.radius = 10
        self.health = 20
        self.damage = 5
        self.speed = 1
        self.is_passive = True  # New flag for passive mobs
        
    def update(self, players):
        # Passive wandering behavior
        self.x += random.uniform(-self.speed, self.speed)
        self.y += random.uniform(-self.speed, self.speed)

class Bee(Monster):
    image = None  # Will be set by GameEngine
    
    def __init__(self, x, y):
        super().__init__(x, y)
        self.radius = 25
        self.health = 30
        self.damage = 8
        self.speed = 1.5
        self.is_passive = True
        self.name = "Bee"
        self.color = (255, 241, 0)  # Yellow fallback color
    
    def render(self, screen):
        if self.image is not None:
            try:
                angle = self.get_angle_to_target()
                rotated_image = pygame.transform.rotate(self.image, -angle)
                screen.blit(rotated_image, 
                           (int(self.x - rotated_image.get_width()/2), 
                            int(self.y - rotated_image.get_height()/2)))
            except:
                # Fallback to circle if rotation fails
                pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        else:
            # Fallback to circle if no image
            pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        
        # Draw health bar
        self.draw_health_bar(screen, self.x, self.y - 45, self.health, 30)
  