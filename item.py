import pygame
import math

class Item:
    def __init__(self, name, color, damage=10, radius=10, max_health=100, image_path=None, image=None):
        self.item_colors = {'basic': (255, 255, 255),
                            'blueberries': (0, 0, 255),
                            'square': (255, 255, 0),
                            'rock': (169, 169, 169),
                            'petal': (255, 255, 128),
                            'moon': (128, 128, 128),
                            'gambler': (255, 0, 0),
                            'beetle': (0, 0, 255)
                            }
        self.name = name
        self.color = self.item_colors[name.lower()]
        self.damage = damage
        self.radius = 10
        self.max_health = max_health
        self.health = max_health  # Current health
        self.image_path = "items/" + self.name.lower() + ".png"
        self.image = image  # Will be loaded by GameEngine
        
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
        self.image = item.image
        self.x = x
        self.y = y
        self.radius = 15
        self.bob_offset = 0
        self.bob_speed = 0.1

    def update(self):
        # Make the item bob up and down
        self.bob_offset = math.sin(pygame.time.get_ticks() * self.bob_speed) * 5

    def render(self, surface):
        if self.image:
            surface.blit(self.image, (int(self.x - self.radius), int(self.y + self.bob_offset - self.radius)))

class RockItem(Item):
    def __init__(self):
        super().__init__("Rock", (169, 169, 169), damage=0, radius=10, max_health=1, image_path="items/rock.png")
