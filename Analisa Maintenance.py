import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

# Set random seed for reproducibility
np.random.seed(42)

# Simulasi 30 hari x 24 jam
hours = 30 * 24
time = pd.date_range(start="2025-05-01", periods=hours, freq='H')

# Generate dummy data
voltage = np.random.normal(loc=220, scale=3, size=hours)  # normal voltage
temperature = np.random.normal(loc=24.5, scale=0.4, size=hours)  # normal temp

# Sisipkan beberapa data abnormal
abnormal_indices = np.random.choice(hours, size=50, replace=False)
voltage[abnormal_indices[:25]] += np.random.uniform(10, 20, size=25)  # high voltage
temperature[abnormal_indices[25:]] += np.random.uniform(2, 5, size=25)  # high temp

# Buat DataFrame
df = pd.DataFrame({
    'time': time,
    'voltage': voltage,
    'temperature': temperature
})

# Parameter Normal
VOLTAGE_MIN = 209
VOLTAGE_MAX = 231
TEMP_MIN = 24
TEMP_MAX = 25

# Klasifikasi kondisi
def classify(row):
    v_ok = VOLTAGE_MIN <= row['voltage'] <= VOLTAGE_MAX
    t_ok = TEMP_MIN <= row['temperature'] <= TEMP_MAX
    if v_ok and t_ok:
        return 'Normal'
    elif not v_ok and not t_ok:
        return 'Highly Abnormal'
    else:
        return 'Abnormal'

df['status'] = df.apply(classify, axis=1)

# Hitung statistik performa
normal_count = (df['status'] == 'Normal').sum()
abnormal_count = (df['status'] == 'Abnormal').sum()
high_abnormal_count = (df['status'] == 'Highly Abnormal').sum()

# Total jam
total_hours = len(df)

# Penurunan lifetime (1 jam abnormal = -1 jam, highly abnormal = -2 jam)
lifetime_loss = abnormal_count * 1 + high_abnormal_count * 2
lifetime_total = 8760  # 1 tahun
lifetime_left = lifetime_total - lifetime_loss
lifetime_percent_left = (lifetime_left / lifetime_total) * 100

# Persentase status
normal_percent = (normal_count / total_hours) * 100
abnormal_percent = ((abnormal_count + high_abnormal_count) / total_hours) * 100

# Tampilkan hasil
print("=== Analisis Performa Motor Selama 1 Bulan ===")
print(f"Total jam data      : {total_hours} jam")
print(f"Normal              : {normal_count} jam ({normal_percent:.2f}%)")
print(f"Abnormal            : {abnormal_count} jam")
print(f"Highly Abnormal     : {high_abnormal_count} jam")
print(f"Total penurunan lifetime: {lifetime_loss} jam")
print(f"Persentase lifetime tersisa: {lifetime_percent_left:.2f}%")

# Visualisasi
plt.figure(figsize=(12, 5))
df['status'].value_counts().plot(kind='bar', color=['green', 'orange', 'red'])
plt.title("Distribusi Status Motor Selama 1 Bulan")
plt.ylabel("Jumlah Jam")
plt.xticks(rotation=0)
plt.grid(axis='y', linestyle='--', alpha=0.7)
plt.tight_layout()
plt.show()