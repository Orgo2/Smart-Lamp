# LED Driver - Updated Command Format

## Overview
The LED driver has been updated with a simplified and more intuitive command format. The driver maintains the state of all 30 LEDs and only updates the LEDs you specify in each command, while still sending data to all LEDs as required by the SK6812RGBW datasheet.

## Changes Made

### 1. Merged Power On and Init
- **Before**: Required separate calls to `LED_Init()` and `LED_PowerOn()`
- **Now**: `LED_Init()` automatically turns on power and waits 500ms for stabilization
- This makes sense because there's no point initializing when power is off

### 2. New Command Format
The new format is more intuitive and supports:
- Setting multiple LEDs with one command
- Individual color channels (R, G, B, W)
- Turning off specific LEDs or all LEDs

## Command Format

### Set LED Color and Intensity
```
LED(led_numbers)&COLOR(intensity)
```

**Examples:**
- `LED(1,5,10)&R(100)` - Set LEDs 1, 5, and 10 to Red with intensity 100
- `LED(2,8,15)&G(50)` - Set LEDs 2, 8, and 15 to Green with intensity 50
- `LED(3,7,20)&B(200)` - Set LEDs 3, 7, and 20 to Blue with intensity 200
- `LED(4,12,25)&W(150)` - Set LEDs 4, 12, and 25 to White with intensity 150

**Parameters:**
- `led_numbers`: Comma-separated list of LED positions (1-30)
- `COLOR`: R (Red), G (Green), B (Blue), or W (White)
- `intensity`: Value from 0 to 255

### Turn Off Specific LEDs
```
LED(led_numbers)&OFF
```

**Examples:**
- `LED(1,8,9)&OFF` - Turn off LEDs 1, 8, and 9 (other LEDs remain in their current state)
- `LED(5,10,15,20)&OFF` - Turn off LEDs 5, 10, 15, and 20

### Turn Off All LEDs (Keep Power On)
```
LED(OFF)
```

This turns off all LEDs but keeps the power supply running. Useful for quickly clearing all LEDs without power cycling.

## State Management

The driver now **maintains the state of all LEDs**:
- When you set LED 1 to Red with `LED(1)&R(100)`
- Then set LED 5 to Green with `LED(5)&G(50)`
- LED 1 remains Red, and LED 5 is Green
- All other LEDs remain in their previous state

This is per the SK6812RGBW datasheet requirement - all 30 LEDs are sent every time, but the driver remembers and preserves the state of LEDs you don't modify.

## API Functions

### Initialization
```c
HAL_StatusTypeDef LED_Init(TIM_HandleTypeDef *htim, uint32_t channel);
```
- Turns on LED power supply
- Waits 500ms for power stabilization
- Clears all LEDs (sets them to off)
- Sends initial state to LED strip

**Example:**
```c
LED_Init(&htim2, TIM_CHANNEL_1);
```

### Command Parsing
```c
HAL_StatusTypeDef LED_ParseCommand(const char *cmd);
```
- Parses and executes LED commands
- Supports all command formats listed above
- Returns `HAL_OK` on success, `HAL_ERROR` on invalid command

**Example:**
```c
LED_ParseCommand("LED(1,5,10)&R(100)");
LED_ParseCommand("LED(2,8)&G(50)");
LED_ParseCommand("LED(3)&B(200)");
LED_ParseCommand("LED(4,5,6)&OFF");
LED_ParseCommand("LED(OFF)");
```

### Power Off
```c
HAL_StatusTypeDef LED_PowerOff(void);
```
- Turns off all LEDs
- Cuts power to the LED strip
- Call this when you want to completely shut down the LED system

### Turn Off All LEDs
```c
HAL_StatusTypeDef LED_AllOff(void);
```
- Turns off all LEDs
- Keeps power supply running
- Useful for quickly clearing all LEDs

### Get State
```c
LED_State_t LED_GetState(void);
```
Returns:
- `LED_STATE_IDLE` - Ready for commands
- `LED_STATE_BUSY` - Currently sending data
- `LED_STATE_POWER_ON_DELAY` - Waiting for power stabilization
- `LED_STATE_ERROR` - Error occurred

## Usage Examples

### Example 1: Basic Setup
```c
// Initialize driver (turns on power automatically)
LED_Init(&htim2, TIM_CHANNEL_1);

// Set LED 1 to Red
LED_ParseCommand("LED(1)&R(255)");

// Set LEDs 5, 10, 15 to Blue
LED_ParseCommand("LED(5,10,15)&B(128)");

// Turn off LED 1 (LEDs 5, 10, 15 remain Blue)
LED_ParseCommand("LED(1)&OFF");
```

### Example 2: Creating Patterns
```c
// Set first 5 LEDs to Red
LED_ParseCommand("LED(1)&R(100)");
LED_ParseCommand("LED(2)&R(100)");
LED_ParseCommand("LED(3)&R(100)");
LED_ParseCommand("LED(4)&R(100)");
LED_ParseCommand("LED(5)&R(100)");

// Set next 5 LEDs to Green
LED_ParseCommand("LED(6,7,8,9,10)&G(100)");

// Mix colors - add Blue to some Red LEDs
LED_ParseCommand("LED(1,3,5)&B(50)");
```

### Example 3: Chaining Commands
```c
// Build a complex pattern by chaining commands
LED_ParseCommand("LED(1,5,10)&R(255)");     // Red LEDs
LED_ParseCommand("LED(2,6,11)&G(255)");     // Green LEDs
LED_ParseCommand("LED(3,7,12)&B(255)");     // Blue LEDs
LED_ParseCommand("LED(4,8,13)&W(255)");     // White LEDs

// Turn off specific pattern
LED_ParseCommand("LED(1,2,3,4)&OFF");

// Clear everything
LED_ParseCommand("LED(OFF)");
```

### Example 4: Dynamic Color Adjustment
```c
// Gradually increase Red intensity
for (int i = 0; i <= 255; i += 10) {
    char cmd[32];
    sprintf(cmd, "LED(1,2,3)&R(%d)", i);
    LED_ParseCommand(cmd);
    HAL_Delay(50);
}

// Fade to White
for (int i = 0; i <= 255; i += 10) {
    char cmd[32];
    sprintf(cmd, "LED(1,2,3)&W(%d)", i);
    LED_ParseCommand(cmd);
    HAL_Delay(50);
}
```

## Important Notes

1. **LED Numbering**: LEDs are numbered from 1 to 30 (not 0 to 29)

2. **State Preservation**: The driver remembers the state of all LEDs. When you update one LED, the others keep their previous values.

3. **Datasheet Compliance**: The driver always sends data to all 30 LEDs as required by the SK6812RGBW datasheet, even when you only modify one.

4. **Color Mixing**: You can set different color channels independently:
   ```c
   LED_ParseCommand("LED(1)&R(100)");  // LED 1 is Red
   LED_ParseCommand("LED(1)&G(50)");   // LED 1 is now Red + Green (Yellow-ish)
   LED_ParseCommand("LED(1)&B(75)");   // LED 1 is now Red + Green + Blue
   ```

5. **Power Management**: 
   - `LED_Init()` turns power ON automatically
   - `LED_PowerOff()` turns power OFF
   - `LED(OFF)` turns LEDs off but keeps power ON

6. **Command Chaining**: Commands can be executed one after another. The driver maintains state between commands.

## Error Handling

The driver returns:
- `HAL_OK` - Command executed successfully
- `HAL_ERROR` - Invalid command format or parameters
- `HAL_BUSY` - Driver is currently busy sending data

Always check return values:
```c
if (LED_ParseCommand("LED(1,5,10)&R(100)") != HAL_OK) {
    // Handle error
}
```
