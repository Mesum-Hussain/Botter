```owl
---
spec-version: "owl-1"
title: "Interpreter test"
---
STEP 1: GATHER
1. ASK USER "What is your name?"
2. SAVE answer INTO VARIABLE $name
3. EXECUTE tool $shell_exec with payload { "command": "echo tool-ran" }
4. SAVE result INTO VARIABLE $out
5. SET $items TO ["a", "b", "c"]

STEP 2: WORK
FOR EACH $it IN $items:
    IF $it IS EQUAL TO "b":
        Mention the letter $it.
IF the user $name seems happy:
    SET $mood TO "happy"
ELSE:
    SET $mood TO "sad"
RETRY UP TO 2 TIMES:
    EXECUTE tool $fs_read with payload { "path": "missing.txt" }
SAVE $items TO FILE "out/items.json"

STEP 3: DONE
RETURN "Hi " + $name + ", mood " + $mood + ", " + $items.length + " items"
```
