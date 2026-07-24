/*
 * Template custom GATT service for Zephyr BLE peripheral roles.
 */

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>

/* Replace UUID bytes with your project UUIDs. */
static struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(
    0xf0, 0xde, 0xbc, 0x9a,
    0x78, 0x56,
    0x34, 0x12,
    0x34, 0x12,
    0x78, 0x56, 0x34, 0x12, 0xcd, 0xab
);

static struct bt_uuid_128 chrc_uuid = BT_UUID_INIT_128(
    0xf1, 0xde, 0xbc, 0x9a,
    0x78, 0x56,
    0x34, 0x12,
    0x34, 0x12,
    0x78, 0x56, 0x34, 0x12, 0xcd, 0xab
);

static uint16_t sample_value;

static ssize_t read_sample(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    const uint16_t *value = attr->user_data;
    uint16_t le = sys_cpu_to_le16(*value);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &le, sizeof(le));
}

BT_GATT_SERVICE_DEFINE(custom_svc,
    BT_GATT_PRIMARY_SERVICE(&svc_uuid),
    BT_GATT_CHARACTERISTIC(&chrc_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           read_sample, NULL, &sample_value)
);
