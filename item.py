import pygame
import math

class Item:
    def __init__(self, name, color, damage=10, radius=10, max_health=100, image_path=None):
        self.name = name
        self.color = color
        self.damage = damage
        self.radius = radius
        self.max_health = max_health
        self.health = max_health  # Current health
        self.image_path = image_path
        self.image = None  # Will be loaded by GameEngine
        
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
        if self.item.image:
            # Draw the item image with bobbing effect
            surface.blit(self.item.image, 
                (int(self.x - self.item.image.get_width()/2), 
                 int(self.y + self.bob_offset - self.item.image.get_height()/2)))
        else:
            # Fallback to circle rendering
            glow_radius = self.radius + 5
            pygame.draw.circle(surface, (255, 255, 255, 128), 
                             (int(self.x), int(self.y + self.bob_offset)), 
                             glow_radius)
            pygame.draw.circle(surface, self.item.color, 
                             (int(self.x), int(self.y + self.bob_offset)), 
                             self.radius)

class RockItem(Item):
    def __init__(self):
        super().__init__("Rock", (169, 169, 169), damage=0, radius=5, max_health=1)