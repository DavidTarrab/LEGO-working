import re
text = """
-Move Closer
-Move Closer
- High Punch
- Move Closer
- Megafireball
- Megafireball
- Megafireball
- Mega fireball
- Mega fireball
- Mega fireball
-
"""
matches = re.findall(r"- ([\w ]+)", text)  
print(matches)
