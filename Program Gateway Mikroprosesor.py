import asyncio  # Library untuk asynchronous programming
import json  # Library untuk mengelola data JSON
from datetime import datetime  # Untuk mendapatkan waktu dan tanggal saat ini
import aiomysql  # Library async untuk koneksi MySQL
from asyncio_mqtt import Client, MqttError  # Library MQTT berbasis asyncio
import socket  # Untuk cek koneksi internet

# Konfigurasi MQTT lokal
LOCAL_BROKER = "192.168.1.100"
LOCAL_TOPIC = "DATA/LOCAL/SENSOR/PANEL_1"

# Konfigurasi MQTT cloud (HiveMQ)
ONLINE_BROKER = "5de3065f0ebb48d986135895199984d6.s2.eu.hivemq.cloud"
ONLINE_PORT = 8883
ONLINE_TOPIC = "DATA/ONLINE/SENSOR/PANEL_1"
MQTT_USERNAME = "embedded_test"
MQTT_PASSWORD = "Ravelware1402"
CLIENT_ID = "gateway1"

# Konfigurasi koneksi MySQL
MYSQL_CONFIG = {
    "host": "localhost",
    "user": "root",
    "password": "",
    "db": "sensor_db"
}

# SQL untuk menyimpan data sensor
INSERT_SQL = """
    INSERT INTO sensor_data (voltage, current, power, temperature, fan_status, timestamp)
    VALUES (%s, %s, %s, %s, %s, %s)
"""

# Fungsi untuk cek koneksi internet
async def check_internet(host="8.8.8.8", port=53, timeout=3):
    try:
        socket.setdefaulttimeout(timeout)
        socket.socket(socket.AF_INET, socket.SOCK_STREAM).connect((host, port))
        return True
    except Exception:
        return False

# Fungsi simpan data ke database MySQL
async def save_to_mysql(pool, data):
    async with pool.acquire() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                INSERT_SQL,
                (
                    data["v"],
                    data["i"],
                    float(str(data["pa"]).replace(",", ".")),
                    data["temp"],
                    data["fan"],
                    data["time"]
                )
            )
            await conn.commit()

# Fungsi kirim data ke cloud MQTT
async def forward_to_cloud(data):
    try:
        async with Client(
            ONLINE_BROKER,
            port=ONLINE_PORT,
            username=MQTT_USERNAME,
            password=MQTT_PASSWORD,
            client_id=CLIENT_ID,
            tls_context=None  # Gunakan SSLContext jika perlu keamanan ketat
        ) as client:
            payload = json.dumps(data)  # Ubah dict ke string JSON
            await client.publish(ONLINE_TOPIC, payload, qos=1)
            print("Data forwarded to cloud.")
    except MqttError as e:
        print("MQTT Publish error:", e)

# Fungsi untuk menangani pesan dari broker lokal
async def handle_local_message(pool, message):
    try:
        data = json.loads(message.payload.decode())  # Decode payload JSON
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")  # Ambil waktu saat ini

        # Tambahkan metadata
        data["status"] = "OK"
        data["deviceID"] = "yourname"
        data["data"]["time"] = timestamp

        await save_to_mysql(pool, data["data"])  # Simpan data sensor
        await forward_to_cloud(data)  # Kirim ke cloud

        print(f"Data received & processed at {timestamp}: {data}")
    except Exception as e:
        print("Error handling message:", e)

# Fungsi utama menjalankan gateway
async def main():
    if not await check_internet():
        print("No internet connection. Trying again...")
        return

    pool = await aiomysql.create_pool(**MYSQL_CONFIG)  # Buat koneksi pool MySQL

    try:
        async with Client(LOCAL_BROKER) as client:  # Koneksi ke broker lokal
            async with client.unfiltered_messages() as messages:
                await client.subscribe(LOCAL_TOPIC)  # Subscribe ke topic lokal
                print(f"Subscribed to local topic: {LOCAL_TOPIC}")
                async for message in messages:  # Loop pesan yang masuk
                    await handle_local_message(pool, message)  # Proses pesan
    except MqttError as e:
        print("MQTT Connection error:", e)
    finally:
        pool.close()  # Tutup koneksi pool
        await pool.wait_closed()

# Jalankan program
if __name__ == "__main__":
    asyncio.run(main())