
#ifndef ESP_PLATFORM
    #error "ring buffer is part of esp-idf FreeRTOS supplemental feature"
#else
    #include "freertos/ringbuf.h"
#endif

namespace espidf {

class RingBuffer {

  public:

    void create(size_t sz, RingbufferType_t type, UBaseType_t cap = MALLOC_CAP_DEFAULT) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 1)
        h =  xRingbufferCreateWithCaps(sz, type, cap); // >=5.2.1
#else
        h = xRingbufferCreate(sz, type);
#endif
        total_size = sz;
        high_watermark = 0;
    }

    void create(size_t sz, RingbufferType_t type,
                uint8_t* pucRingbufferStorage,
                StaticRingbuffer_t* pxStaticRingbuffer) {
        h = xRingbufferCreateStatic(sz, type, pucRingbufferStorage, pxStaticRingbuffer);
        total_size = sz;
        high_watermark = 0;
    }

    void free() {
        vRingbufferDelete(h);
    }

    size_t max_item_size() const {
        return xRingbufferGetMaxItemSize(h);
    }

    size_t curr_free_size() const {
        return xRingbufferGetCurFreeSize(h);
    }

    BaseType_t send(void* pvItem, size_t xItemSize, TickType_t xTicksToWait) {
        return xRingbufferSend(h, pvItem, xItemSize, xTicksToWait);
    }

    BaseType_t send_from_isr(void* pvItem, size_t xItemSize, BaseType_t *pxHigherPriorityTaskWoke) {
        return xRingbufferSendFromISR(h, pvItem, xItemSize, pxHigherPriorityTaskWoke);
    }

    BaseType_t send_acquire(void **ppvItem, size_t xItemSize, TickType_t xTicksToWait) {
        return xRingbufferSendAcquire(h, ppvItem, xItemSize, xTicksToWait);
    }

    BaseType_t send_complete(void *pvItem) {
        return xRingbufferSendComplete(h, pvItem);
    }

    void* receive(size_t* sz,  TickType_t xTicksToWait) {
        return xRingbufferReceive(h, sz, xTicksToWait);
    }

    void return_item(void* pvItem) {
        vRingbufferReturnItem(h, pvItem);
    }

    void* receive_from_isr(size_t* sz) {
        return xRingbufferReceiveFromISR(h, sz);
    }

    void return_item_from_isr(void* pvItem, BaseType_t *pxHigherPriorityTaskWoken) {
        vRingbufferReturnItemFromISR(h, pvItem, pxHigherPriorityTaskWoken);
    }

    size_t get_current_usage() const {
        return total_size - xRingbufferGetCurFreeSize(h);
    }

    size_t get_high_watermark() const {
        return high_watermark;
    }

    size_t get_total_size() const {
        return total_size;
    }

    void update_high_watermark() {
        size_t current_usage = get_current_usage();
        if (current_usage > high_watermark) {
            high_watermark = current_usage;
        }
    }

    void reset_high_watermark() {
        high_watermark = 0;
    }

    operator RingbufHandle_t() const {
        return h;
    }

  private:
    RingbufHandle_t h;
    size_t total_size;
    size_t high_watermark;

};

}
