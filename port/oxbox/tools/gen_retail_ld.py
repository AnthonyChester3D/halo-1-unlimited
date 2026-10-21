"""port/oxbox/retail_syms.txt -> port/oxbox/retail.ld (symbol = address; lines starting retail_mainXapi are patch sites)"""
out = ["/* generated from retail_syms.txt: the retail XBE's library symbols at their addresses */"]
for l in open('port/oxbox/retail_syms.txt'):
    if l.startswith('#') or not l.strip(): continue
    n, a = l.split()[:2]
    if not n.startswith('retail_mainXapi'): out.append(f'{n} = {a};')
open('port/oxbox/retail.ld', 'w').write('\n'.join(out) + '\n')
