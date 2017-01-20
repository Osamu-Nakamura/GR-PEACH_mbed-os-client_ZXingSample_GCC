/*
 * Copyright (c) 2015, 2016 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "simpleclient.h"
#include <string>
#include <sstream>
#include <vector>
#include "mbed-trace/mbed_trace.h"
#include "mbedtls/entropy_poll.h"

#include "security.h"

#include "mbed.h"
#include "rtos.h"

#if MBED_CONF_APP_NETWORK_INTERFACE == WIFI
    #if(1) //bp3595
        #include "LWIPBP3595Interface.h"
        LWIPBP3595Interface wifi;
        DigitalOut usb1en(P3_8);
    #endif
#elif MBED_CONF_APP_NETWORK_INTERFACE == ETHERNET
    #include "EthernetInterface.h"
    EthernetInterface eth;
#endif

#define MBED_SERVER_ADDRESS "coap://api.connector.mbed.com:5684"

RawSerial output(USBTX, USBRX);

// These are example resource values for the Device Object
struct MbedClientDevice device = {
    "Manufacturer_String",      // Manufacturer
    "Type_String",              // Type
    "ModelNumber_String",       // ModelNumber
    "SerialNumber_String"       // SerialNumber
};

// Instantiate the class which implements LWM2M Client API (from simpleclient.h)
MbedClient mbed_client(device);

#if(1)  //for zxing with camera
InterruptIn unreg_button(USER_BUTTON0);
#else
// In case of K64F board , there is button resource available
// to change resource value and unregister
#ifdef TARGET_K64F
// Set up Hardware interrupt button.
InterruptIn obs_button(SW2);
InterruptIn unreg_button(SW3);
#else
//In non K64F boards , set up a timer to simulate updating resource,
// there is no functionality to unregister.
Ticker timer;
#endif
#endif

// LED Output
DigitalOut led1(LED1);
DigitalOut led2(LED2);
DigitalOut led3(LED3);

/*
 * Arguments for running "blink" in it's own thread.
 */
class BlinkArgs {
public:
    BlinkArgs() {
        clear();
    }
    void clear() {
        position = 0;
        blink_pattern.clear();
    }
    uint16_t position;
    std::vector<uint32_t> blink_pattern;
};

/*
 * The Led contains one property (pattern) and a function (blink).
 * When the function blink is executed, the pattern is read, and the LED
 * will blink based on the pattern.
 */

class LedResource {
public:
    LedResource() {
        // create ObjectID with metadata tag of '3201', which is 'digital output'
        led_object = M2MInterfaceFactory::create_object("3201");
        M2MObjectInstance* led_inst = led_object->create_object_instance();

        // 5855 = Multi-state output
        M2MResource* color_res = led_inst->create_dynamic_resource("5855", "Color",
            M2MResourceInstance::STRING, false);
        // read and write
        color_res->set_operation(M2MBase::GET_PUT_ALLOWED);
        // set red as initial color
        color_res->set_value((const uint8_t*)"red", 3);
        
        // 5853 = Multi-state output
        M2MResource* pattern_res = led_inst->create_dynamic_resource("5853", "Pattern",
            M2MResourceInstance::STRING, false);
        // read and write
        pattern_res->set_operation(M2MBase::GET_PUT_ALLOWED);
        // set initial pattern (toggle every 200ms. 7 toggles in total)
        pattern_res->set_value((const uint8_t*)"500:500:500:500:500:500:500", 27);

        // there's not really an execute LWM2M ID that matches... hmm...
        M2MResource* led_res = led_inst->create_dynamic_resource("5850", "Blink",
            M2MResourceInstance::OPAQUE, false);
        // we allow executing a function here...
        led_res->set_operation(M2MBase::POST_ALLOWED);
        // when a POST comes in, we want to execute the led_execute_callback
        led_res->set_execute_function(execute_callback(this, &LedResource::blink));
    }

    M2MObject* get_object() {
        return led_object;
    }

    void blink(void *) {
        // read the value of 'Pattern'
        M2MObjectInstance* inst = led_object->object_instance();
        M2MResource* res = inst->resource("5853");
        // read the value of 'Color'
        M2MObjectInstance* instC = led_object->object_instance();
        M2MResource* resC = instC->resource("5855");

        // values in mbed Client are all buffers, and we need a vector of int's
        uint8_t* buffIn = NULL;
        uint32_t sizeIn;
        res->get_value(buffIn, sizeIn);
        
        uint8_t* cbuffIn = NULL;
        uint32_t csizeIn;
        resC->get_value(cbuffIn, csizeIn);

        // turn the buffer into a string, and initialize a vector<int> on the heap
        std::string s((char*)buffIn, sizeIn);
        std::vector<uint32_t>* v = new std::vector<uint32_t>;

        output.printf("led_execute_callback pattern=%s\r\n", s.c_str());

        // our pattern is something like 500:200:500, so parse that
        std::size_t found = s.find_first_of(":");
        while (found!=std::string::npos) {

            v->push_back(atoi((const char*)s.substr(0,found).c_str()));
            s = s.substr(found+1);
            found=s.find_first_of(":");
            if(found == std::string::npos) {
                v->push_back(atoi((const char*)s.c_str()));
            }
        }


        // do_blink is called with the vector, and starting at -1
        do_blink(v, cbuffIn, 0);
    }

private:
    M2MObject* led_object;

    void do_blink(std::vector<uint32_t>* pattern, uint8_t* color,  uint16_t position) {
        
        if (!strcmp((char *)color, "red")) {
            // blink the LED in red color
            led1 = !led1;
        }
        else if (!strcmp((char *)color, "green")) {
            // blink in green color
            led2 = !led2;
        }
        else if (!strcmp((char *)color, "blue")) {
            // blink in blue color
            led3 = !led3;
        }
        else if (!strcmp((char *)color, "cyan")) {
            // blink in cyan color
            led2 = !led2;
            led3 = !led3;
        }
        else if (!strcmp((char *)color, "yellow")) {
            // blink in yellow color
            led1 = !led1;
            led2 = !led2;
        }
        else if (!strcmp((char *)color, "magenta")) {
            // blink in magenta color
            led1 = !led1;
            led3 = !led3;
        }            
        else if (!strcmp((char *)color, "white")) {
            // blink in white color
            led1 = !led1;
            led2 = !led2;
            led3 = !led3;
        }
        else {
            // no operation
        }

        // up the position, if we reached the end of the vector
        if (position >= pattern->size()) {
            // free memory, and exit this function
            delete pattern;
            return;
        }

        // how long do we need to wait before the next blink?
        uint32_t delay_ms = pattern->at(position);

        // Invoke same function after `delay_ms` (upping position)
        Thread::wait(delay_ms);
        do_blink(pattern, color, ++position);
    }
};

#if(1)  //for zxing with camera
/*
 * The Zxing contains a function (send string).
 * When `handle_string_send` is executed, the string after decoding is sent.
 */
class ZxingResource {
public:
    ZxingResource() {
        // create ObjectID with metadata tag of '3202', which is 'send string'
        zxing_object = M2MInterfaceFactory::create_object("3202");
        M2MObjectInstance* zxing_inst = zxing_object->create_object_instance();
        // create resource with ID '5700', which is 'send string'
        M2MResource* zxing_res = zxing_inst->create_dynamic_resource("5700", "zxing",
            M2MResourceInstance::STRING, true);
        // we can read this value
        zxing_res->set_operation(M2MBase::GET_ALLOWED);
        // set initial value (all values in mbed Client are buffers)
        // to be able to read this data easily in the Connector console, we'll use a string
        zxing_res->set_value((uint8_t*)"0", 1);        
    }

    ~ZxingResource() {
    }

    M2MObject* get_object() {
        return zxing_object;
    }

    /*
     * When you success the decode process of barcode, we send the string after decoding to mbed Device Connector.
     */
    void handle_string_send(char * addr, int size) {
        M2MObjectInstance* inst = zxing_object->object_instance();
        M2MResource* res = inst->resource("5700");

        printf("%s\r\n", addr);

        // tell the string to connector
        res->set_value((uint8_t *)addr, size);
    }

private:
    M2MObject* zxing_object;
};
#else
/*
 * The button contains one property (click count).
 * When `handle_button_click` is executed, the counter updates.
 */
class ButtonResource {
public:
    ButtonResource(): counter(0) {
        // create ObjectID with metadata tag of '3200', which is 'digital input'
        btn_object = M2MInterfaceFactory::create_object("3200");
        M2MObjectInstance* btn_inst = btn_object->create_object_instance();
        // create resource with ID '5501', which is digital input counter
        M2MResource* btn_res = btn_inst->create_dynamic_resource("5501", "Button",
            M2MResourceInstance::INTEGER, true /* observable */);
        // we can read this value
        btn_res->set_operation(M2MBase::GET_ALLOWED);
        // set initial value (all values in mbed Client are buffers)
        // to be able to read this data easily in the Connector console, we'll use a string
        btn_res->set_value((uint8_t*)"0", 1);
    }

    ~ButtonResource() {
    }

    M2MObject* get_object() {
        return btn_object;
    }

    /*
     * When you press the button, we read the current value of the click counter
     * from mbed Device Connector, then up the value with one.
     */
    void handle_button_click() {
        M2MObjectInstance* inst = btn_object->object_instance();
        M2MResource* res = inst->resource("5501");

        // up counter
        counter++;
#ifdef TARGET_K64F
        printf("handle_button_click, new value of counter is %d\r\n", counter);
#else
        printf("simulate button_click, new value of counter is %d\r\n", counter);
#endif
        // serialize the value of counter as a string, and tell connector
        char buffer[20];
        int size = sprintf(buffer,"%d",counter);
        res->set_value((uint8_t*)buffer, size);
    }

private:
    M2MObject* btn_object;
    uint16_t counter;
};
#endif

class BigPayloadResource {
public:
    BigPayloadResource() {
        big_payload = M2MInterfaceFactory::create_object("1000");
        M2MObjectInstance* payload_inst = big_payload->create_object_instance();
        M2MResource* payload_res = payload_inst->create_dynamic_resource("1", "BigData",
            M2MResourceInstance::STRING, true /* observable */);
        payload_res->set_operation(M2MBase::GET_PUT_ALLOWED);
        payload_res->set_value((uint8_t*)"0", 1);
        payload_res->set_incoming_block_message_callback(
                    incoming_block_message_callback(this, &BigPayloadResource::block_message_received));
        payload_res->set_outgoing_block_message_callback(
                    outgoing_block_message_callback(this, &BigPayloadResource::block_message_requested));
    }

    M2MObject* get_object() {
        return big_payload;
    }

    void block_message_received(M2MBlockMessage *argument) {
        if (argument) {
            if (M2MBlockMessage::ErrorNone == argument->error_code()) {
                if (argument->is_last_block()) {
                    output.printf("Last block received\r\n");
                }
                output.printf("Block number: %d\r\n", argument->block_number());
                // First block received
                if (argument->block_number() == 0) {
                    // Store block
                // More blocks coming
                } else {
                    // Store blocks
                }
            } else {
                output.printf("Error when receiving block message!  - EntityTooLarge\r\n");
            }
            output.printf("Total message size: %d\r\n", argument->total_message_size());
        }
    }

    void block_message_requested(const String& resource, uint8_t *&/*data*/, uint32_t &/*len*/) {
        output.printf("GET request received for resource: %s\r\n", resource.c_str());
        // Copy data and length to coap response
    }

private:
    M2MObject*  big_payload;
};

// Network interaction must be performed outside of interrupt context
#if(1)  //for zxing with camera
ZxingResource zxing_resource;
#else
Semaphore updates(0);
#endif
volatile bool registered = false;
volatile bool clicked = false;
osThreadId mainThread;

void unregister() {
    registered = false;
#if(1)  //for zxing with camera
#else
    updates.release();
#endif
}

#if(1)  //for zxing with camera
#else
void button_clicked() {
    clicked = true;
    updates.release();
}
#endif

// debug printf function
void trace_printer(const char* str) {
    printf("%s\r\n", str);
}

// Status indication
Ticker status_ticker;
DigitalOut status_led(LED4);
void blinky() { status_led = !status_led; }

#if(1)  //for zxing with camera
extern void zxing_init(void (*pfunc)(char * addr, int size));
extern int zxing_loop();

static void callback_zxing(char * addr, int size) {
    zxing_resource.handle_string_send(addr, size);
}
#endif

// Entry point to the program
int main() {

    unsigned int seed;
    size_t len;

#ifdef MBEDTLS_ENTROPY_HARDWARE_ALT
    // Used to randomize source port
    mbedtls_hardware_poll(NULL, (unsigned char *) &seed, sizeof seed, &len);

#elif defined MBEDTLS_TEST_NULL_ENTROPY

#warning "mbedTLS security feature is disabled. Connection will not be secure !! Implement proper hardware entropy for your selected hardware."
    // Used to randomize source port
    mbedtls_null_entropy_poll( NULL,(unsigned char *) &seed, sizeof seed, &len);

#else

#error "This hardware does not have entropy, endpoint will not register to Connector.\
You need to enable NULL ENTROPY for your application, but if this configuration change is made then no security is offered by mbed TLS.\
Add MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES and MBEDTLS_TEST_NULL_ENTROPY in mbed_app.json macros to register your endpoint."

#endif

    srand(seed);
    status_ticker.attach_us(blinky, 250000);
    // Keep track of the main thread
    mainThread = osThreadGetId();

    // Sets the console baud-rate
    output.baud(115200);

    output.printf("\r\nStarting mbed Client example in ");
#if defined (MESH) || (MBED_CONF_LWIP_IPV6_ENABLED==true)
    output.printf("IPv6 mode\r\n");
#else
    output.printf("IPv4 mode\r\n");
#endif

    mbed_trace_init();
    mbed_trace_print_function_set(trace_printer);

    NetworkInterface *network_interface = 0;
    int connect_success = -1;
#if MBED_CONF_APP_NETWORK_INTERFACE == WIFI
    output.printf("\n\rConnecting to WiFi...\r\n");
#if(1) //bp3595
    usb1en = 1;
    Thread::wait(5);
    usb1en = 0;
    Thread::wait(5);
    connect_success = wifi.connect(MBED_CONF_APP_WIFI_SSID, MBED_CONF_APP_WIFI_PASSWORD, MBED_CONF_APP_WIFI_SECURITY);
#endif
    network_interface = &wifi;
#elif MBED_CONF_APP_NETWORK_INTERFACE == ETHERNET
    output.printf("\n\rConnecting to ethernet...\r\n");
    connect_success = eth.connect();
    network_interface = &eth;
#endif
    if(connect_success == 0) {
    output.printf("\n\rConnected to Network successfully\r\n");
    } else {
        output.printf("\n\rConnection to Network Failed %d! Exiting application....\r\n", connect_success);
        return 0;
    }
    const char *ip_addr = network_interface->get_ip_address();
    if (ip_addr) {
        output.printf("IP address %s\r\n", ip_addr);
    } else {
        output.printf("No IP address\r\n");
    }

    // we create our button and LED resources
#if(1)  //for zxing with camera
    LedResource led_resource;

    // On press of USER_BUTTON0 button on GR-PEACH board, example application
    // will call unregister API towards mbed Device Connector
    unreg_button.fall(&unregister);
#else
    ButtonResource button_resource;
    LedResource led_resource;
    BigPayloadResource big_payload_resource;

#ifdef TARGET_K64F
    // On press of SW3 button on K64F board, example application
    // will call unregister API towards mbed Device Connector
    //unreg_button.fall(&mbed_client,&MbedClient::test_unregister);
    unreg_button.fall(&unregister);

    // Observation Button (SW2) press will send update of endpoint resource values to connector
    obs_button.fall(&button_clicked);
#else
    // Send update of endpoint resource values to connector every 15 seconds periodically
    timer.attach(&button_clicked, 15.0);
#endif
#endif

    // Create endpoint interface to manage register and unregister
    mbed_client.create_interface(MBED_SERVER_ADDRESS, network_interface);

    // Create Objects of varying types, see simpleclient.h for more details on implementation.
    M2MSecurity* register_object = mbed_client.create_register_object(); // server object specifying connector info
    M2MDevice*   device_object   = mbed_client.create_device_object();   // device resources object

    // Create list of Objects to register
    M2MObjectList object_list;

    // Add objects to list
    object_list.push_back(device_object);
#if(1)  //for zxing with camera
    object_list.push_back(zxing_resource.get_object());
#else
    object_list.push_back(button_resource.get_object());
#endif
    object_list.push_back(led_resource.get_object());
//    object_list.push_back(big_payload_resource.get_object());

    // Set endpoint registration object
    mbed_client.set_register_object(register_object);

    // Register with mbed Device Connector
    mbed_client.test_register(register_object, object_list);
    registered = true;

#if(1)
    zxing_init(&callback_zxing);
    Timer update_timer;
    update_timer.reset();
    update_timer.start();

    while (registered) {
        if (zxing_loop() == 0) {
            update_timer.reset();
        } else if (update_timer.read() >= 25) {
            mbed_client.test_update_register();
            update_timer.reset();
        } else {
            // do nothing
        }
        Thread::wait(5);
    }
#else
    while (true) {
        updates.wait(25000);
        if(registered) {
            if(!clicked) {
                mbed_client.test_update_register();
            }
        }else {
            break;
        }
        if(clicked) {
           clicked = false;
            button_resource.handle_button_click();
        }
    }
#endif
    mbed_client.test_unregister();
    status_ticker.detach();
}
