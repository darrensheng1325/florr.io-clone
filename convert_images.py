import os
from PIL import Image
from cairosvg import svg2png
from io import BytesIO

def convert_and_resize_images(input_dir='items', output_dir='items', size=(40, 40)):
    """
    Convert SVG/WebP files to PNG and resize them to specified size.
    Also resize existing PNG files to the specified size.
    """
    # Create output directory if it doesn't exist
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    # Process all files in input directory
    for filename in os.listdir(input_dir):
        input_path = os.path.join(input_dir, filename)
        
        # Skip directories
        if os.path.isdir(input_path):
            continue
        
        # Get file extension
        _, ext = os.path.splitext(filename)
        ext = ext.lower()
        
        # Create output filename (always .png)
        output_filename = os.path.splitext(filename)[0] + '.png'
        output_path = os.path.join(output_dir, output_filename)
        
        try:
            if ext == '.svg':
                # Convert SVG to PNG with high DPI for quality
                print(f"Converting {filename} from SVG to PNG...")
                with open(input_path, 'rb') as svg_file:
                    svg_data = svg_file.read()
                png_data = svg2png(svg_data, dpi=300)
                image = Image.open(BytesIO(png_data))
                
            elif ext in ['.png', '.webp']:
                # Open PNG or WebP file
                print(f"Opening {ext} file: {filename}")
                image = Image.open(input_path)
                
                # Convert WebP to RGBA mode if necessary
                if image.mode != 'RGBA':
                    image = image.convert('RGBA')
                
            else:
                print(f"Skipping unsupported file: {filename}")
                continue
            
            # Resize image
            print(f"Resizing {filename} to {size}...")
            resized_image = image.resize(size, Image.Resampling.LANCZOS)
            
            # Save as PNG with transparency
            resized_image.save(output_path, 'PNG', optimize=True)
            print(f"Saved {output_filename}")
            
        except Exception as e:
            print(f"Error processing {filename}: {e}")

if __name__ == "__main__":
    # Convert and resize all images to 40x40
    convert_and_resize_images()
    print("Image conversion complete!") 