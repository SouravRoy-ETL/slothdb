import pyarrow.parquet as pq
import os
os.chdir(r'C:\Users\soura\Documents\lightdb')
f = pq.ParquetFile('bench/clickbench/data/hits.parquet')
schema = f.schema_arrow
url_idx = None
for i, name in enumerate(schema.names):
    if name == 'URL':
        url_idx = i
        break
print(f'URL col idx={url_idx}', flush=True)

total_uncomp = 0
total_comp = 0
plain_pages = 0
dict_pages = 0
for rg_idx in range(f.num_row_groups):
    rg = f.metadata.row_group(rg_idx)
    col = rg.column(url_idx)
    encs = list(col.encodings)
    has_plain = 'PLAIN' in encs
    has_dict = 'PLAIN_DICTIONARY' in encs or 'RLE_DICTIONARY' in encs
    total_uncomp += col.total_uncompressed_size
    total_comp += col.total_compressed_size
    if rg_idx < 5:
        print(f'RG{rg_idx}: encodings={encs} uncomp={col.total_uncompressed_size/1e6:.1f}MB comp={col.total_compressed_size/1e6:.1f}MB num_values={col.num_values}', flush=True)

print(f'Total: uncomp={total_uncomp/1e9:.2f}GB comp={total_comp/1e9:.2f}GB', flush=True)
print(f'Compression ratio: {total_uncomp/total_comp:.2f}x', flush=True)

# Check URL encoding distribution across all RGs
plain_rgs = 0
dict_only_rgs = 0
mixed_rgs = 0
for rg_idx in range(f.num_row_groups):
    rg = f.metadata.row_group(rg_idx)
    col = rg.column(url_idx)
    encs = set(col.encodings)
    has_plain = 'PLAIN' in encs
    has_dict = 'PLAIN_DICTIONARY' in encs or 'RLE_DICTIONARY' in encs
    if has_plain and has_dict:
        mixed_rgs += 1
    elif has_dict:
        dict_only_rgs += 1
    elif has_plain:
        plain_rgs += 1
print(f'RGs: total={f.num_row_groups} plain_only={plain_rgs} dict_only={dict_only_rgs} mixed={mixed_rgs}', flush=True)
