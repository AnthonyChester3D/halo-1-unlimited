"""port/oxbox/retail_syms.txt -> port/oxbox/retail.ld (symbol = address; lines starting retail_mainXapi are patch sites)"""
out = ["/* generated from retail_syms.txt: the retail XBE's library symbols at their addresses */"]
for l in open('port/oxbox/retail_syms.txt'):
    if l.startswith('#') or not l.strip(): continue
    n, a = l.split()[:2]
    if n.startswith('retail_mainXapi'): continue
    # (XNet and Winsock: PROVIDEd, so the online builds' own, port/oxbox/net/oxbox_lwip_ws.c, take their place)
    if n.startswith(('XNet', 'WSA', '__WSA', 'retail_XNet', 'retail_ws_', 'halo_ws_')): out.append(f'PROVIDE({n} = {a});')
    else: out.append(f'{n} = {a};')
open('port/oxbox/retail.ld', 'w').write('\n'.join(out) + '\n')
