#include "address.h"

uint32_t addr_page_size(uint32_t vpn)
{
    return (vpn % 2 == 0) ? PAGE_SIZE_EVEN : PAGE_SIZE_ODD;
}

uint32_t addr_page_base(uint32_t vpn)
{
    return (vpn / 2) * PAGE_PAIR_SIZE + (vpn % 2) * PAGE_SIZE_EVEN;
}

uint32_t addr_frames_needed(uint32_t vpn)
{
    return addr_page_size(vpn) / FRAME_SIZE;
}

bool addr_translate(uint32_t virtual_address, translation_t *out)
{
    if (virtual_address >= VIRTUAL_MEMORY_SIZE || out == NULL) {
        return false;
    }

    uint32_t pair = virtual_address / PAGE_PAIR_SIZE;
    uint32_t inside_pair = virtual_address % PAGE_PAIR_SIZE;
    bool odd = inside_pair >= PAGE_SIZE_EVEN;

    out->virtual_address = virtual_address;
    out->vpn = 2 * pair + (odd ? 1 : 0);
    out->offset = odd ? inside_pair - PAGE_SIZE_EVEN : inside_pair;
    out->page_size = odd ? PAGE_SIZE_ODD : PAGE_SIZE_EVEN;
    return true;
}

uint32_t addr_physical(const page_table_entry_t *pte, uint32_t offset)
{
    return pte->frame_number * FRAME_SIZE + offset;
}

void addr_log(FILE *out, uint32_t virtual_address)
{
    translation_t t;

    if (!addr_translate(virtual_address, &t)) {
        fprintf(out, "[MMU] VA %7u -> INVALIDO (limite de %u bytes)\n",
                virtual_address, (unsigned)VIRTUAL_MEMORY_SIZE);
        return;
    }
    fprintf(out, "[MMU] VA %7u -> VPN %3u | Offset %4u | Pagina de %u KB (%s)\n",
            t.virtual_address, t.vpn, t.offset, t.page_size / 1024,
            (t.vpn % 2 == 0) ? "par" : "impar");
}
