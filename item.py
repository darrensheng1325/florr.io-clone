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
                            'beetle': (0, 0, 255),
                            'leaf': (0, 255, 0)}
        self.item_damages = {'basic': 10,
                            'blueberries': 10,
                            'square': 1,
                            'rock': 5,
                            'petal': 1,
                            'moon': 2,
                            'gambler': 50,
                            'beetle': 20,
                            'leaf': 10}
        self.item_healths = {'basic': 100,
                              'blueberries': 100,
                              'square': 50,
                              'rock': 200,
                              'petal': 100,
                              'moon': 1000,
                              'gambler': 1,
                              'beetle': 50,
                              'leaf': 100}
        self.name = name
        self.color = self.item_colors[name.lower()]
        self.damage = self.item_damages[name.lower()]
        self.radius = 10
        self.max_health = self.item_healths[name.lower()]
        self.health = self.max_health  # Current health
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

class LeafItem(Item):
    def __init__(self):
        super().__init__("Leaf", (0, 255, 0), damage=10, radius=10, max_health=100, image_path="items/leaf.png", image=pygame.image.load("items/leaf.png").convert_alpha())

