#include "mbed.h"

DigitalOut din(p15);
DigitalOut clk(p16);
DigitalOut le(p17);

I2CSlave slave(p9, p10);    // An I2C Slave, used for communicating with an I2C Master device
DigitalOut led_read(LED1);
DigitalOut led_write(LED2);

enum CUBE_CONSTANTS {
    CUBE_LEVELS = 5,
    DATA_SIZE = 32,
    DATA_BYTES = 20,        // Number of bytes for a full cube pattern
    CUBE_LEVEL_DELAY = 4    // ms (4 seems to be minimum)
};

enum I2C_CONSTANTS {
    I2C_SLAVE_ADDRESS = 0x90,       // 0x48 on linux
    I2C_FREQUENCY = 100000,
    I2C_BUFFER_SIZE = 21            // Command + 4bytes x 5 levels
};

enum I2C_COMMANDS {
    WRITE_CUBE = 0x00
};

void outputData(unsigned int data)
{
    for (int i = 0; i < DATA_SIZE; i++) {
        clk = 0;
        din = ((data >> i) & 0x0000001);
        clk = 1;
    }
    clk = 0;
    le = 1;
    wait_us(1);
    le = 0;
}

int main()
{
    // Ping pong buffering
    unsigned int cubebuffer_0[CUBE_LEVELS] = {   // 32 bits per level and 5 levels
        0xF0000001,                     // Some hardcoded test pattern
        0xE8000002,
        0xD8000004,
        0xB8000008,
        0x78000010
    };
    unsigned int cubebuffer_1[CUBE_LEVELS] = {   // 32 bits per level and 5 levels
        0xF0000001,                     // Some hardcoded test pattern
        0xE8000002,
        0xD8000004,
        0xB8000008,
        0x78000010
    };
    unsigned int * currentbuffer = cubebuffer_0;
    unsigned int * otherbuffer = cubebuffer_1;
    unsigned char ilevel = 0;
    bool switchbuffers = false;

    // I2C
    char buffer[I2C_BUFFER_SIZE];
    int i2cstatus;

    // Disable cube
    clk = 0;
    din = 0;
    le = 0;
    outputData(0xF8000000);     // Clear cube
    printf("Mbed cube started and cleared. Awaiting commands\r\n");

    // Configure slave
    slave.frequency(I2C_FREQUENCY);
    slave.address(I2C_SLAVE_ADDRESS);

    // Eternity
    while(1)
    {
        outputData(currentbuffer[ilevel]);

        i2cstatus = slave.receive();
        switch (i2cstatus)
        {
            case I2CSlave::WriteAddressed:      // The master is writing to this slave
                if (slave.read(buffer, I2C_BUFFER_SIZE) == 0)
                {
                    led_write = !led_write;

                    switch (buffer[0])      // Command byte
                    {
                        case WRITE_CUBE:    // Write cube pattern
                            memcpy((char *)otherbuffer, (buffer+1), DATA_BYTES);
                            switchbuffers = true;
                            break;

                        default:
                            printf("Unknown command byte\r\n");
                            break;
                    }
                }
                else {
                    printf("Could not retrieve i2c data\r\n");
                }
                break;

            case I2CSlave::ReadAddressed:       // the master has requested a read from this slave
                if (slave.read(buffer, I2C_BUFFER_SIZE) == 0) {
                    led_read = !led_read;
                    printf("Read request received.\r\n");
                }
                else {
                    printf("Read request received. FAIL.\r\n");
                }
                break;

            default:
                break;
        }

        // Clear I2C buffer
        for (int i = 0; i < I2C_BUFFER_SIZE; i++) {
            buffer[i] = 0;
        }

        ilevel = (ilevel + 1) % CUBE_LEVELS;

        // Switch buffers
        if (ilevel == 0 && switchbuffers) {
            unsigned int * tmp = currentbuffer;
            currentbuffer = otherbuffer;
            otherbuffer = tmp;
            switchbuffers = false;
        }
    }
}
