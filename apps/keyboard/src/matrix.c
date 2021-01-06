#include "hal/hal_gpio.h"
#include "os/os.h"

#include "matrix.h"

#if (MATRIX_COLS <= 8)
#    define print_matrix_header()  print("\nr/c 01234567\n")
#    define print_matrix_row(row)  print_bin_reverse8(matrix_get_row(row))
#    define matrix_bitpop(i)       bitpop(matrix[i])
#    define ROW_SHIFTER ((uint8_t)1)
#elif (MATRIX_COLS <= 16)
#    define print_matrix_header()  print("\nr/c 0123456789ABCDEF\n")
#    define print_matrix_row(row)  print_bin_reverse16(matrix_get_row(row))
#    define matrix_bitpop(i)       bitpop16(matrix[i])
#    define ROW_SHIFTER ((uint16_t)1)
#elif (MATRIX_COLS <= 32)
#    define print_matrix_header()  print("\nr/c 0123456789ABCDEF0123456789ABCDEF\n")
#    define print_matrix_row(row)  print_bin_reverse32(matrix_get_row(row))
#    define matrix_bitpop(i)       bitpop32(matrix[i])
#    define ROW_SHIFTER  ((uint32_t)1)
#endif

static const int row_pins[MATRIX_ROWS] = MYNEWT_VAL(TMK_MATRIX_ROW_PINS);
static const int col_pins[MATRIX_COLS] = MYNEWT_VAL(TMK_MATRIX_COL_PINS);
static matrix_row_t matrix[MATRIX_ROWS];

static bool read_cols_on_row(matrix_row_t current_matrix[], uint8_t current_row);
static void unselect_row(int row);
static void select_row(int row);

void
matrix_init(void)
{
    for (int x = 0; x < MATRIX_ROWS; x++) {
        int pin = row_pins[x];
        hal_gpio_init_out(pin, 0);
    }

    for (int x = 0; x < MATRIX_COLS; x++) {
        int pin = col_pins[x];
        hal_gpio_init_in(pin, HAL_GPIO_PULL_UP);
    }
}

static void
select_row(int row)
{
    int pin = row_pins[row];
    hal_gpio_write(pin, 0);
}

static void
unselect_row(int row)
{
    int pin = row_pins[row];
    hal_gpio_write(pin, 1);
}

static bool
read_cols_on_row(matrix_row_t current_matrix[], uint8_t current_row)
{
    /* Store last value of row prior to reading */
    matrix_row_t last_row_value = current_matrix[current_row];

    /* Clear data in matrix row */
    current_matrix[current_row] = 0;

    /* Select row and wait for row selecton to stabilize */
    select_row(current_row);
    os_cputime_delay_usecs(30);

    /* For each col... */
    for (uint8_t col_index = 0; col_index < MATRIX_COLS; col_index++) {

        /* Select the col pin to read (active low) */
        int pin = col_pins[col_index];
        int pin_state = hal_gpio_read(pin);

        /* Populate the matrix row with the state of the col pin */
        current_matrix[current_row] |= pin_state ? 0 : (ROW_SHIFTER << col_index);
    }

    /* Unselect row */
    unselect_row(current_row);

    return (last_row_value != current_matrix[current_row]);
}

uint8_t
matrix_scan(void)
{
    /* Set row, read cols */
    for (int current_row = 0; current_row < MATRIX_ROWS; current_row++) {
        read_cols_on_row(matrix, current_row);
    }
    return 0;
}

matrix_row_t
matrix_get_row(uint8_t row)
{
    // Matrix mask lets you disable switches in the returned matrix data. For example, if you have a
    // switch blocker installed and the switch is always pressed.
    return matrix[row];
}
