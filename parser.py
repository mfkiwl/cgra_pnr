from __future__ import print_function


# parse the ones generated by metapath2vec
def parse_emb(filename, filter_hyperedge=True):
    with open(filename) as f:
        lines = f.readlines()
    lines = lines[2:]
    input_data = {}
    num_dim = 0
    for line in lines:
        raw_data = line.split()
        if num_dim == 0:
            num_dim = len(raw_data) - 1
        else:
            assert(len(raw_data) == num_dim + 1)
        netid = raw_data[0]
        if filter_hyperedge and netid[0] == "e":
            continue
        inputs = [float(x) for x in raw_data[1:]]
        input_data[netid] = inputs
    return num_dim, input_data




