import pandas as pd


def parse(path):
    def process(path):
        with open(path) as f:
            for line in f:
                line = iter(line.strip().split('\t'))
                keys = ['time', 'host', 'level', 'subject']
                data = {k: v for k, v in zip(keys, line)}

                for v in line:
                    v = v.split('=', maxsplit=1)
                    if len(v) == 2:
                        k, v = v
                    else:
                        k, v = '_', v[0]

                    while k in data:
                        k = '_' + k

                    data[k] = v

                yield data

    data = pd.DataFrame(process(path))
    data.time = pd.to_datetime(data.time)
    data['dt'] = (data.time - data.time.min()).dt.total_seconds()
    data = data.sort_values(by=['time', 'host'])
    data.index = range(len(data))
    cols = ['time', 'dt', 'host', 'level', 'subject']
    cols += [x for x in sorted(data.columns) if x not in cols]

    return data[cols]


def lol():
    pass
