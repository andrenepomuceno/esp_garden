import pandas as pd
import matplotlib.pyplot as plt

def plotFunction(data, name):
    fig, ax = plt.subplots()
    ax.plot(data, lw=0.5)
    fig.autofmt_xdate()
    plt.savefig(name)
    plt.clf()
    
def process(name):
    print(f"processing {name}...")
    data = fieldList[name].resample(sample_rate).mean()
    plotFunction(data, name + '.svg')

df = pd.read_csv(
    'feeds(3).csv',
    sep=',',
    index_col=['created_at'],
    parse_dates=['created_at'],
    infer_datetime_format=True,
    verbose=True,
    dtype={'status': str})

df = df[('2023' & df.field3 < 10e6)]
sample_rate = '1h'

fieldList = {
    'moisture': df.field1,
    'ping': df.field3,
    'waterLevel': df.field4,
    'luminosity': df.field5,
    'temperature': df.field6,
    'airHumidity': df.field7
}

for field in fieldList:
    process(field)
