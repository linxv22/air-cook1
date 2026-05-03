#ifndef __LCD_H
#define __LCD_H

// LVGL library is not thread-safe, this example will call LVGL APIs from different tasks, so use a mutex to protect it


// LCD function declarations
void LCD_Init(void);


#endif // __LCD_H