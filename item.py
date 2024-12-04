import pygame
import math

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