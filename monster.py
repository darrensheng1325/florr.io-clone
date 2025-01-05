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
        self.knockback_resistance = 0.95
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
        self.speed = 0
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
    image = None  # Will be set by GameEngine
    
    def __init__(self, x, y):
        super().__init__(x, y)
        self.radius = 15
        self.health = 20
        self.damage = 5
        self.speed = 1
        self.is_passive = True
        self.name = "Ant"
        self.color = (139, 69, 19)  # Brown fallback color
        self.angle = random.uniform(0, 360)  # Random initial angle
        self.max_health = 20
        self.is_aggressive = False
        self.aggro_speed = 2
        self.direction_timer = random.uniform(0, 3)  # Timer for direction changes
        self.move_direction = pygame.math.Vector2(1, 0).rotate(random.uniform(0, 360))
        
    def update(self, players):
        if self.is_aggressive:
            # Aggressive behavior - chase nearest player
            super().update(players)
        else:
            # Update direction timer
            self.direction_timer -= 0.016  # Assuming 60 FPS
            if self.direction_timer <= 0:
                # Change direction randomly
                self.move_direction = pygame.math.Vector2(1, 0).rotate(random.uniform(0, 360))
                self.direction_timer = random.uniform(2, 4)  # New random timer
                self.angle = math.degrees(math.atan2(self.move_direction.y, self.move_direction.x))
            
            # Move in current direction with slight randomness
            self.x += self.move_direction.x * self.speed + random.uniform(-0.2, 0.2)
            self.y += self.move_direction.y * self.speed + random.uniform(-0.2, 0.2)

    def render(self, screen):
        if self.image:
            rotated_image = pygame.transform.rotate(self.image, -self.angle)
            screen.blit(rotated_image, 
                       (int(self.x - rotated_image.get_width()/2), 
                        int(self.y - rotated_image.get_height()/2)))
        else:
            # Fallback to circle if no image
            pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        
        # Draw health bar
        self.draw_health_bar(screen, self.x, self.y - 25, self.health, self.max_health)

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
        self.direction_timer = random.uniform(0, 3)
        self.move_direction = pygame.math.Vector2(1, 0).rotate(random.uniform(0, 360))
        self.hover_offset = random.uniform(0, 2 * math.pi)  # For hovering effect
    
    def update(self, players):
        # Update direction timer
        self.direction_timer -= 0.016
        if self.direction_timer <= 0:
            # Change direction randomly, but favor upward/downward movement for bees
            angle = random.uniform(-60, 60)  # More vertical movement
            self.move_direction = pygame.math.Vector2(1, 0).rotate(angle)
            self.direction_timer = random.uniform(1, 3)  # Shorter timer for more frequent changes
        
        # Add hovering motion
        hover = math.sin(time.time() * 3 + self.hover_offset) * 0.5
        
        # Move in current direction with hovering effect
        self.x += self.move_direction.x * self.speed + random.uniform(-0.1, 0.1)
        self.y += self.move_direction.y * self.speed + hover + random.uniform(-0.1, 0.1)
        
        # Update angle for rendering
        self.angle = math.degrees(math.atan2(self.move_direction.y, self.move_direction.x))
    
    def render(self, screen):
        if self.image is not None:
            try:
                rotated_image = pygame.transform.rotate(self.image, -self.angle - 90)
                screen.blit(rotated_image, 
                           (int(self.x - rotated_image.get_width()/2), 
                            int(self.y - rotated_image.get_height()/2)))
            except:
                pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        else:
            pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        
        self.draw_health_bar(screen, self.x, self.y - 45, self.health, 30)

class Bird(Monster):
    image = None  # Will be set by GameEngine
    
    def __init__(self, x, y):
        super().__init__(x, y)
        self.radius = 30
        self.health = 80
        self.damage = 15
        self.speed = 2.5
        self.name = "Bird"
        self.color = (74, 74, 74)  # Gray fallback color
        self.direction_timer = random.uniform(0, 3)
        self.move_direction = pygame.math.Vector2(1, 0).rotate(random.uniform(0, 360))
        self.hover_offset = random.uniform(0, 2 * math.pi)
        self.swooping = False
        self.swoop_target_x = 0
        self.swoop_target_y = 0
        self.swoop_speed = 4
        self.swoop_cooldown = 0
    
    def update(self, players):
        # Update swoop cooldown
        if self.swoop_cooldown > 0:
            self.swoop_cooldown -= 0.016  # Assuming 60 FPS
        
        if self.swooping:
            # Move towards swoop target at high speed
            dx = self.swoop_target_x - self.x
            dy = self.swoop_target_y - self.y
            distance = math.sqrt(dx * dx + dy * dy)
            
            if distance > 0:
                self.x += (dx / distance) * self.swoop_speed
                self.y += (dy / distance) * self.swoop_speed
            
            # End swoop if we're close to target
            if distance < 10:
                self.swooping = False
                self.swoop_cooldown = 3  # 3 second cooldown
        else:
            # Normal movement with random direction changes
            self.direction_timer -= 0.016
            if self.direction_timer <= 0:
                # Change direction randomly
                self.move_direction = pygame.math.Vector2(1, 0).rotate(random.uniform(0, 360))
                self.direction_timer = random.uniform(1, 2)  # Shorter timer for more agile movement
                
                # Maybe start a swoop if cooldown is ready
                if self.swoop_cooldown <= 0 and random.random() < 0.3:  # 30% chance to swoop
                    nearest_x, nearest_y = self.find_nearest_player(players)
                    self.swooping = True
                    self.swoop_target_x = nearest_x
                    self.swoop_target_y = nearest_y
            
            # Add slight hovering motion
            hover = math.sin(time.time() * 2 + self.hover_offset) * 0.3
            
            # Move in current direction with hovering effect
            self.x += self.move_direction.x * self.speed + random.uniform(-0.1, 0.1)
            self.y += self.move_direction.y * self.speed + hover + random.uniform(-0.1, 0.1)
        
        # Update angle for rendering - bird always faces movement direction
        if self.swooping:
            self.angle = math.degrees(math.atan2(
                self.swoop_target_y - self.y,
                self.swoop_target_x - self.x
            ))
        else:
            self.angle = math.degrees(math.atan2(
                self.move_direction.y,
                self.move_direction.x
            ))
    
    def render(self, screen):
        if self.image is not None:
            try:
                # Rotate image to face movement direction
                rotated_image = pygame.transform.rotate(self.image, -self.angle - 90)
                screen.blit(rotated_image, 
                           (int(self.x - rotated_image.get_width()/2), 
                            int(self.y - rotated_image.get_height()/2)))
            except:
                pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        else:
            pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        
        # Draw health bar
        self.draw_health_bar(screen, self.x, self.y - 45, self.health, 80)

class Boss(Monster):
    image = None  # Will be set by GameEngine
    
    def __init__(self, x, y):
        super().__init__(x, y, health=8000)
        self.color = (255, 0, 0)  # Red
        self.speed = 1.5  # 50% of player speed (assuming player speed is 3)
        self.radius = 200  # 10x player size (player is ~20 radius)
        self.knockback_resistance = 0.99  # Very resistant to knockback
        self.damage = 10
        self.name = "Boss"
        self.max_health = 8000
        
    def render(self, screen):
        if self.image:
            angle = self.get_angle_to_target()
            # Scale the image to 10x size
            scaled_image = pygame.transform.scale(self.image, (400, 400))  # 10x normal size
            rotated_image = pygame.transform.rotate(scaled_image, -angle - 90)
            screen.blit(rotated_image, 
                       (int(self.x - rotated_image.get_width()/2), 
                        int(self.y - rotated_image.get_height()/2)))
        else:
            # Fallback to circle if no image
            pygame.draw.circle(screen, self.color, (int(self.x), int(self.y)), self.radius)
        
        # Draw health bar above boss
        self.draw_health_bar(screen, self.x, self.y - self.radius - 20, self.health, self.max_health)
  