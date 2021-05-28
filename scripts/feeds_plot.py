import pandas as pd
import matplotlib.pyplot as plt

def plot_function(data, name):
    fig, ax = plt.subplots()
    ax.plot(data, lw=0.5)
    fig.autofmt_xdate()
    plt.savefig(name)
    plt.clf()

df = pd.read_csv('feeds.csv', parse_dates=['created_at'], index_col=['created_at'])

sample_rate = '30T'
moisture = df['field1'].resample(sample_rate).mean()
plot_function(moisture, 'moisture.svg')

luminosity = df['field2'].resample(sample_rate).mean()
plot_function(luminosity, 'luminosity.svg')

temperature = df['field6'].resample(sample_rate).mean()
plot_function(temperature, 'temperature.svg')

air_humidity = df['field7'].resample(sample_rate).mean()
plot_function(air_humidity, 'air_humidity.svg')
