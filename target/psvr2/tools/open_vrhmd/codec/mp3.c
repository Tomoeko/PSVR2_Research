#include "mp3.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ISO/IEC 11172-3, Annex B: numeric coding parameters only. Decoder code
 * below implements the specified transforms directly. No external decoder. */
typedef struct { uint32_t code; uint8_t bits, value; } Mp3Code;
static const Mp3Code codes_1[4] = {
    {1u,1u,0u}, {1u,3u,1u}, {1u,2u,16u}, {0u,3u,17u},
};
static const Mp3Code codes_2[9] = {
    {1u,1u,0u}, {2u,3u,1u}, {1u,6u,2u}, {3u,3u,16u}, {1u,3u,17u}, {1u,5u,18u}, {3u,5u,32u}, {2u,5u,33u},
    {0u,6u,34u},
};
static const Mp3Code codes_3[9] = {
    {3u,2u,0u}, {2u,2u,1u}, {1u,6u,2u}, {1u,3u,16u}, {1u,2u,17u}, {1u,5u,18u}, {3u,5u,32u}, {2u,5u,33u},
    {0u,6u,34u},
};
static const Mp3Code codes_5[16] = {
    {1u,1u,0u}, {2u,3u,1u}, {6u,6u,2u}, {5u,7u,3u}, {3u,3u,16u}, {1u,3u,17u}, {4u,6u,18u}, {4u,7u,19u},
    {7u,6u,32u}, {5u,6u,33u}, {7u,7u,34u}, {1u,8u,35u}, {6u,7u,48u}, {1u,6u,49u}, {1u,7u,50u}, {0u,8u,51u},
};
static const Mp3Code codes_6[16] = {
    {7u,3u,0u}, {3u,3u,1u}, {5u,5u,2u}, {1u,7u,3u}, {6u,3u,16u}, {2u,2u,17u}, {3u,4u,18u}, {2u,5u,19u},
    {5u,4u,32u}, {4u,4u,33u}, {4u,5u,34u}, {1u,6u,35u}, {3u,6u,48u}, {3u,5u,49u}, {2u,6u,50u}, {0u,7u,51u},
};
static const Mp3Code codes_7[36] = {
    {1u,1u,0u}, {2u,3u,1u}, {10u,6u,2u}, {19u,8u,3u}, {16u,8u,4u}, {10u,9u,5u}, {3u,3u,16u}, {3u,4u,17u},
    {7u,6u,18u}, {10u,7u,19u}, {5u,7u,20u}, {3u,8u,21u}, {11u,6u,32u}, {4u,5u,33u}, {13u,7u,34u}, {17u,8u,35u},
    {8u,8u,36u}, {4u,9u,37u}, {12u,7u,48u}, {11u,7u,49u}, {18u,8u,50u}, {15u,9u,51u}, {11u,9u,52u}, {2u,9u,53u},
    {7u,7u,64u}, {6u,7u,65u}, {9u,8u,66u}, {14u,9u,67u}, {3u,9u,68u}, {1u,10u,69u}, {6u,8u,80u}, {4u,8u,81u},
    {5u,9u,82u}, {3u,10u,83u}, {2u,10u,84u}, {0u,10u,85u},
};
static const Mp3Code codes_8[36] = {
    {3u,2u,0u}, {4u,3u,1u}, {6u,6u,2u}, {18u,8u,3u}, {12u,8u,4u}, {5u,9u,5u}, {5u,3u,16u}, {1u,2u,17u},
    {2u,4u,18u}, {16u,8u,19u}, {9u,8u,20u}, {3u,8u,21u}, {7u,6u,32u}, {3u,4u,33u}, {5u,6u,34u}, {14u,8u,35u},
    {7u,8u,36u}, {3u,9u,37u}, {19u,8u,48u}, {17u,8u,49u}, {15u,8u,50u}, {13u,9u,51u}, {10u,9u,52u}, {4u,10u,53u},
    {13u,8u,64u}, {5u,7u,65u}, {8u,8u,66u}, {11u,9u,67u}, {5u,10u,68u}, {1u,10u,69u}, {12u,9u,80u}, {4u,8u,81u},
    {4u,9u,82u}, {1u,9u,83u}, {1u,11u,84u}, {0u,11u,85u},
};
static const Mp3Code codes_9[36] = {
    {7u,3u,0u}, {5u,3u,1u}, {9u,5u,2u}, {14u,6u,3u}, {15u,8u,4u}, {7u,9u,5u}, {6u,3u,16u}, {4u,3u,17u},
    {5u,4u,18u}, {5u,5u,19u}, {6u,6u,20u}, {7u,8u,21u}, {7u,4u,32u}, {6u,4u,33u}, {8u,5u,34u}, {8u,6u,35u},
    {8u,7u,36u}, {5u,8u,37u}, {15u,6u,48u}, {6u,5u,49u}, {9u,6u,50u}, {10u,7u,51u}, {5u,7u,52u}, {1u,8u,53u},
    {11u,7u,64u}, {7u,6u,65u}, {9u,7u,66u}, {6u,7u,67u}, {4u,8u,68u}, {1u,9u,69u}, {14u,8u,80u}, {4u,7u,81u},
    {6u,8u,82u}, {2u,8u,83u}, {6u,9u,84u}, {0u,9u,85u},
};
static const Mp3Code codes_10[64] = {
    {1u,1u,0u}, {2u,3u,1u}, {10u,6u,2u}, {23u,8u,3u}, {35u,9u,4u}, {30u,9u,5u}, {12u,9u,6u}, {17u,10u,7u},
    {3u,3u,16u}, {3u,4u,17u}, {8u,6u,18u}, {12u,7u,19u}, {18u,8u,20u}, {21u,9u,21u}, {12u,8u,22u}, {7u,8u,23u},
    {11u,6u,32u}, {9u,6u,33u}, {15u,7u,34u}, {21u,8u,35u}, {32u,9u,36u}, {40u,10u,37u}, {19u,9u,38u}, {6u,9u,39u},
    {14u,7u,48u}, {13u,7u,49u}, {22u,8u,50u}, {34u,9u,51u}, {46u,10u,52u}, {23u,10u,53u}, {18u,9u,54u}, {7u,10u,55u},
    {20u,8u,64u}, {19u,8u,65u}, {33u,9u,66u}, {47u,10u,67u}, {27u,10u,68u}, {22u,10u,69u}, {9u,10u,70u}, {3u,10u,71u},
    {31u,9u,80u}, {22u,9u,81u}, {41u,10u,82u}, {26u,10u,83u}, {21u,11u,84u}, {20u,11u,85u}, {5u,10u,86u}, {3u,11u,87u},
    {14u,8u,96u}, {13u,8u,97u}, {10u,9u,98u}, {11u,10u,99u}, {16u,10u,100u}, {6u,10u,101u}, {5u,11u,102u}, {1u,11u,103u},
    {9u,9u,112u}, {8u,8u,113u}, {7u,9u,114u}, {8u,10u,115u}, {4u,10u,116u}, {4u,11u,117u}, {2u,11u,118u}, {0u,11u,119u},
};
static const Mp3Code codes_11[64] = {
    {3u,2u,0u}, {4u,3u,1u}, {10u,5u,2u}, {24u,7u,3u}, {34u,8u,4u}, {33u,9u,5u}, {21u,8u,6u}, {15u,9u,7u},
    {5u,3u,16u}, {3u,3u,17u}, {4u,4u,18u}, {10u,6u,19u}, {32u,8u,20u}, {17u,8u,21u}, {11u,7u,22u}, {10u,8u,23u},
    {11u,5u,32u}, {7u,5u,33u}, {13u,6u,34u}, {18u,7u,35u}, {30u,8u,36u}, {31u,9u,37u}, {20u,8u,38u}, {5u,8u,39u},
    {25u,7u,48u}, {11u,6u,49u}, {19u,7u,50u}, {59u,9u,51u}, {27u,8u,52u}, {18u,10u,53u}, {12u,8u,54u}, {5u,9u,55u},
    {35u,8u,64u}, {33u,8u,65u}, {31u,8u,66u}, {58u,9u,67u}, {30u,9u,68u}, {16u,10u,69u}, {7u,9u,70u}, {5u,10u,71u},
    {28u,8u,80u}, {26u,8u,81u}, {32u,9u,82u}, {19u,10u,83u}, {17u,10u,84u}, {15u,11u,85u}, {8u,10u,86u}, {14u,11u,87u},
    {14u,8u,96u}, {12u,7u,97u}, {9u,7u,98u}, {13u,8u,99u}, {14u,9u,100u}, {9u,10u,101u}, {4u,10u,102u}, {1u,10u,103u},
    {11u,8u,112u}, {4u,7u,113u}, {6u,8u,114u}, {6u,9u,115u}, {6u,10u,116u}, {3u,10u,117u}, {2u,10u,118u}, {0u,10u,119u},
};
static const Mp3Code codes_12[64] = {
    {9u,4u,0u}, {6u,3u,1u}, {16u,5u,2u}, {33u,7u,3u}, {41u,8u,4u}, {39u,9u,5u}, {38u,9u,6u}, {26u,9u,7u},
    {7u,3u,16u}, {5u,3u,17u}, {6u,4u,18u}, {9u,5u,19u}, {23u,7u,20u}, {16u,7u,21u}, {26u,8u,22u}, {11u,8u,23u},
    {17u,5u,32u}, {7u,4u,33u}, {11u,5u,34u}, {14u,6u,35u}, {21u,7u,36u}, {30u,8u,37u}, {10u,7u,38u}, {7u,8u,39u},
    {17u,6u,48u}, {10u,5u,49u}, {15u,6u,50u}, {12u,6u,51u}, {18u,7u,52u}, {28u,8u,53u}, {14u,8u,54u}, {5u,8u,55u},
    {32u,7u,64u}, {13u,6u,65u}, {22u,7u,66u}, {19u,7u,67u}, {18u,8u,68u}, {16u,8u,69u}, {9u,8u,70u}, {5u,9u,71u},
    {40u,8u,80u}, {17u,7u,81u}, {31u,8u,82u}, {29u,8u,83u}, {17u,8u,84u}, {13u,9u,85u}, {4u,8u,86u}, {2u,9u,87u},
    {27u,8u,96u}, {12u,7u,97u}, {11u,7u,98u}, {15u,8u,99u}, {10u,8u,100u}, {7u,9u,101u}, {4u,9u,102u}, {1u,10u,103u},
    {27u,9u,112u}, {12u,8u,113u}, {8u,8u,114u}, {12u,9u,115u}, {6u,9u,116u}, {3u,9u,117u}, {1u,9u,118u}, {0u,10u,119u},
};
static const Mp3Code codes_13[256] = {
    {1u,1u,0u}, {5u,4u,1u}, {14u,6u,2u}, {21u,7u,3u}, {34u,8u,4u}, {51u,9u,5u}, {46u,9u,6u}, {71u,10u,7u},
    {42u,9u,8u}, {52u,10u,9u}, {68u,11u,10u}, {52u,11u,11u}, {67u,12u,12u}, {44u,12u,13u}, {43u,13u,14u}, {19u,13u,15u},
    {3u,3u,16u}, {4u,4u,17u}, {12u,6u,18u}, {19u,7u,19u}, {31u,8u,20u}, {26u,8u,21u}, {44u,9u,22u}, {33u,9u,23u},
    {31u,9u,24u}, {24u,9u,25u}, {32u,10u,26u}, {24u,10u,27u}, {31u,11u,28u}, {35u,12u,29u}, {22u,12u,30u}, {14u,12u,31u},
    {15u,6u,32u}, {13u,6u,33u}, {23u,7u,34u}, {36u,8u,35u}, {59u,9u,36u}, {49u,9u,37u}, {77u,10u,38u}, {65u,10u,39u},
    {29u,9u,40u}, {40u,10u,41u}, {30u,10u,42u}, {40u,11u,43u}, {27u,11u,44u}, {33u,12u,45u}, {42u,13u,46u}, {16u,13u,47u},
    {22u,7u,48u}, {20u,7u,49u}, {37u,8u,50u}, {61u,9u,51u}, {56u,9u,52u}, {79u,10u,53u}, {73u,10u,54u}, {64u,10u,55u},
    {43u,10u,56u}, {76u,11u,57u}, {56u,11u,58u}, {37u,11u,59u}, {26u,11u,60u}, {31u,12u,61u}, {25u,13u,62u}, {14u,13u,63u},
    {35u,8u,64u}, {16u,7u,65u}, {60u,9u,66u}, {57u,9u,67u}, {97u,10u,68u}, {75u,10u,69u}, {114u,11u,70u}, {91u,11u,71u},
    {54u,10u,72u}, {73u,11u,73u}, {55u,11u,74u}, {41u,12u,75u}, {48u,12u,76u}, {53u,13u,77u}, {23u,13u,78u}, {24u,14u,79u},
    {58u,9u,80u}, {27u,8u,81u}, {50u,9u,82u}, {96u,10u,83u}, {76u,10u,84u}, {70u,10u,85u}, {93u,11u,86u}, {84u,11u,87u},
    {77u,11u,88u}, {58u,11u,89u}, {79u,12u,90u}, {29u,11u,91u}, {74u,13u,92u}, {49u,13u,93u}, {41u,14u,94u}, {17u,14u,95u},
    {47u,9u,96u}, {45u,9u,97u}, {78u,10u,98u}, {74u,10u,99u}, {115u,11u,100u}, {94u,11u,101u}, {90u,11u,102u}, {79u,11u,103u},
    {69u,11u,104u}, {83u,12u,105u}, {71u,12u,106u}, {50u,12u,107u}, {59u,13u,108u}, {38u,13u,109u}, {36u,14u,110u}, {15u,14u,111u},
    {72u,10u,112u}, {34u,9u,113u}, {56u,10u,114u}, {95u,11u,115u}, {92u,11u,116u}, {85u,11u,117u}, {91u,12u,118u}, {90u,12u,119u},
    {86u,12u,120u}, {73u,12u,121u}, {77u,13u,122u}, {65u,13u,123u}, {51u,13u,124u}, {44u,14u,125u}, {43u,16u,126u}, {42u,16u,127u},
    {43u,9u,128u}, {20u,8u,129u}, {30u,9u,130u}, {44u,10u,131u}, {55u,10u,132u}, {78u,11u,133u}, {72u,11u,134u}, {87u,12u,135u},
    {78u,12u,136u}, {61u,12u,137u}, {46u,12u,138u}, {54u,13u,139u}, {37u,13u,140u}, {30u,14u,141u}, {20u,15u,142u}, {16u,15u,143u},
    {53u,10u,144u}, {25u,9u,145u}, {41u,10u,146u}, {37u,10u,147u}, {44u,11u,148u}, {59u,11u,149u}, {54u,11u,150u}, {81u,13u,151u},
    {66u,12u,152u}, {76u,13u,153u}, {57u,13u,154u}, {54u,14u,155u}, {37u,14u,156u}, {18u,14u,157u}, {39u,16u,158u}, {11u,15u,159u},
    {35u,10u,160u}, {33u,10u,161u}, {31u,10u,162u}, {57u,11u,163u}, {42u,11u,164u}, {82u,12u,165u}, {72u,12u,166u}, {80u,13u,167u},
    {47u,12u,168u}, {58u,13u,169u}, {55u,14u,170u}, {21u,13u,171u}, {22u,14u,172u}, {26u,15u,173u}, {38u,16u,174u}, {22u,17u,175u},
    {53u,11u,176u}, {25u,10u,177u}, {23u,10u,178u}, {38u,11u,179u}, {70u,12u,180u}, {60u,12u,181u}, {51u,12u,182u}, {36u,12u,183u},
    {55u,13u,184u}, {26u,13u,185u}, {34u,13u,186u}, {23u,14u,187u}, {27u,15u,188u}, {14u,15u,189u}, {9u,15u,190u}, {7u,16u,191u},
    {34u,11u,192u}, {32u,11u,193u}, {28u,11u,194u}, {39u,12u,195u}, {49u,12u,196u}, {75u,13u,197u}, {30u,12u,198u}, {52u,13u,199u},
    {48u,14u,200u}, {40u,14u,201u}, {52u,15u,202u}, {28u,15u,203u}, {18u,15u,204u}, {17u,16u,205u}, {9u,16u,206u}, {5u,16u,207u},
    {45u,12u,208u}, {21u,11u,209u}, {34u,12u,210u}, {64u,13u,211u}, {56u,13u,212u}, {50u,13u,213u}, {49u,14u,214u}, {45u,14u,215u},
    {31u,14u,216u}, {19u,14u,217u}, {12u,14u,218u}, {15u,15u,219u}, {10u,16u,220u}, {7u,15u,221u}, {6u,16u,222u}, {3u,16u,223u},
    {48u,13u,224u}, {23u,12u,225u}, {20u,12u,226u}, {39u,13u,227u}, {36u,13u,228u}, {35u,13u,229u}, {53u,15u,230u}, {21u,14u,231u},
    {16u,14u,232u}, {23u,17u,233u}, {13u,15u,234u}, {10u,15u,235u}, {6u,15u,236u}, {1u,17u,237u}, {4u,16u,238u}, {2u,16u,239u},
    {16u,12u,240u}, {15u,12u,241u}, {17u,13u,242u}, {27u,14u,243u}, {25u,14u,244u}, {20u,14u,245u}, {29u,15u,246u}, {11u,14u,247u},
    {17u,15u,248u}, {12u,15u,249u}, {16u,16u,250u}, {8u,16u,251u}, {1u,19u,252u}, {1u,18u,253u}, {0u,19u,254u}, {1u,16u,255u},
};
static const Mp3Code codes_15[256] = {
    {7u,3u,0u}, {12u,4u,1u}, {18u,5u,2u}, {53u,7u,3u}, {47u,7u,4u}, {76u,8u,5u}, {124u,9u,6u}, {108u,9u,7u},
    {89u,9u,8u}, {123u,10u,9u}, {108u,10u,10u}, {119u,11u,11u}, {107u,11u,12u}, {81u,11u,13u}, {122u,12u,14u}, {63u,13u,15u},
    {13u,4u,16u}, {5u,3u,17u}, {16u,5u,18u}, {27u,6u,19u}, {46u,7u,20u}, {36u,7u,21u}, {61u,8u,22u}, {51u,8u,23u},
    {42u,8u,24u}, {70u,9u,25u}, {52u,9u,26u}, {83u,10u,27u}, {65u,10u,28u}, {41u,10u,29u}, {59u,11u,30u}, {36u,11u,31u},
    {19u,5u,32u}, {17u,5u,33u}, {15u,5u,34u}, {24u,6u,35u}, {41u,7u,36u}, {34u,7u,37u}, {59u,8u,38u}, {48u,8u,39u},
    {40u,8u,40u}, {64u,9u,41u}, {50u,9u,42u}, {78u,10u,43u}, {62u,10u,44u}, {80u,11u,45u}, {56u,11u,46u}, {33u,11u,47u},
    {29u,6u,48u}, {28u,6u,49u}, {25u,6u,50u}, {43u,7u,51u}, {39u,7u,52u}, {63u,8u,53u}, {55u,8u,54u}, {93u,9u,55u},
    {76u,9u,56u}, {59u,9u,57u}, {93u,10u,58u}, {72u,10u,59u}, {54u,10u,60u}, {75u,11u,61u}, {50u,11u,62u}, {29u,11u,63u},
    {52u,7u,64u}, {22u,6u,65u}, {42u,7u,66u}, {40u,7u,67u}, {67u,8u,68u}, {57u,8u,69u}, {95u,9u,70u}, {79u,9u,71u},
    {72u,9u,72u}, {57u,9u,73u}, {89u,10u,74u}, {69u,10u,75u}, {49u,10u,76u}, {66u,11u,77u}, {46u,11u,78u}, {27u,11u,79u},
    {77u,8u,80u}, {37u,7u,81u}, {35u,7u,82u}, {66u,8u,83u}, {58u,8u,84u}, {52u,8u,85u}, {91u,9u,86u}, {74u,9u,87u},
    {62u,9u,88u}, {48u,9u,89u}, {79u,10u,90u}, {63u,10u,91u}, {90u,11u,92u}, {62u,11u,93u}, {40u,11u,94u}, {38u,12u,95u},
    {125u,9u,96u}, {32u,7u,97u}, {60u,8u,98u}, {56u,8u,99u}, {50u,8u,100u}, {92u,9u,101u}, {78u,9u,102u}, {65u,9u,103u},
    {55u,9u,104u}, {87u,10u,105u}, {71u,10u,106u}, {51u,10u,107u}, {73u,11u,108u}, {51u,11u,109u}, {70u,12u,110u}, {30u,12u,111u},
    {109u,9u,112u}, {53u,8u,113u}, {49u,8u,114u}, {94u,9u,115u}, {88u,9u,116u}, {75u,9u,117u}, {66u,9u,118u}, {122u,10u,119u},
    {91u,10u,120u}, {73u,10u,121u}, {56u,10u,122u}, {42u,10u,123u}, {64u,11u,124u}, {44u,11u,125u}, {21u,11u,126u}, {25u,12u,127u},
    {90u,9u,128u}, {43u,8u,129u}, {41u,8u,130u}, {77u,9u,131u}, {73u,9u,132u}, {63u,9u,133u}, {56u,9u,134u}, {92u,10u,135u},
    {77u,10u,136u}, {66u,10u,137u}, {47u,10u,138u}, {67u,11u,139u}, {48u,11u,140u}, {53u,12u,141u}, {36u,12u,142u}, {20u,12u,143u},
    {71u,9u,144u}, {34u,8u,145u}, {67u,9u,146u}, {60u,9u,147u}, {58u,9u,148u}, {49u,9u,149u}, {88u,10u,150u}, {76u,10u,151u},
    {67u,10u,152u}, {106u,11u,153u}, {71u,11u,154u}, {54u,11u,155u}, {38u,11u,156u}, {39u,12u,157u}, {23u,12u,158u}, {15u,12u,159u},
    {109u,10u,160u}, {53u,9u,161u}, {51u,9u,162u}, {47u,9u,163u}, {90u,10u,164u}, {82u,10u,165u}, {58u,10u,166u}, {57u,10u,167u},
    {48u,10u,168u}, {72u,11u,169u}, {57u,11u,170u}, {41u,11u,171u}, {23u,11u,172u}, {27u,12u,173u}, {62u,13u,174u}, {9u,12u,175u},
    {86u,10u,176u}, {42u,9u,177u}, {40u,9u,178u}, {37u,9u,179u}, {70u,10u,180u}, {64u,10u,181u}, {52u,10u,182u}, {43u,10u,183u},
    {70u,11u,184u}, {55u,11u,185u}, {42u,11u,186u}, {25u,11u,187u}, {29u,12u,188u}, {18u,12u,189u}, {11u,12u,190u}, {11u,13u,191u},
    {118u,11u,192u}, {68u,10u,193u}, {30u,9u,194u}, {55u,10u,195u}, {50u,10u,196u}, {46u,10u,197u}, {74u,11u,198u}, {65u,11u,199u},
    {49u,11u,200u}, {39u,11u,201u}, {24u,11u,202u}, {16u,11u,203u}, {22u,12u,204u}, {13u,12u,205u}, {14u,13u,206u}, {7u,13u,207u},
    {91u,11u,208u}, {44u,10u,209u}, {39u,10u,210u}, {38u,10u,211u}, {34u,10u,212u}, {63u,11u,213u}, {52u,11u,214u}, {45u,11u,215u},
    {31u,11u,216u}, {52u,12u,217u}, {28u,12u,218u}, {19u,12u,219u}, {14u,12u,220u}, {8u,12u,221u}, {9u,13u,222u}, {3u,13u,223u},
    {123u,12u,224u}, {60u,11u,225u}, {58u,11u,226u}, {53u,11u,227u}, {47u,11u,228u}, {43u,11u,229u}, {32u,11u,230u}, {22u,11u,231u},
    {37u,12u,232u}, {24u,12u,233u}, {17u,12u,234u}, {12u,12u,235u}, {15u,13u,236u}, {10u,13u,237u}, {2u,12u,238u}, {1u,13u,239u},
    {71u,12u,240u}, {37u,11u,241u}, {34u,11u,242u}, {30u,11u,243u}, {28u,11u,244u}, {20u,11u,245u}, {17u,11u,246u}, {26u,12u,247u},
    {21u,12u,248u}, {16u,12u,249u}, {10u,12u,250u}, {6u,12u,251u}, {8u,13u,252u}, {6u,13u,253u}, {2u,13u,254u}, {0u,13u,255u},
};
static const Mp3Code codes_16[256] = {
    {1u,1u,0u}, {5u,4u,1u}, {14u,6u,2u}, {44u,8u,3u}, {74u,9u,4u}, {63u,9u,5u}, {110u,10u,6u}, {93u,10u,7u},
    {172u,11u,8u}, {149u,11u,9u}, {138u,11u,10u}, {242u,12u,11u}, {225u,12u,12u}, {195u,12u,13u}, {376u,13u,14u}, {17u,9u,15u},
    {3u,3u,16u}, {4u,4u,17u}, {12u,6u,18u}, {20u,7u,19u}, {35u,8u,20u}, {62u,9u,21u}, {53u,9u,22u}, {47u,9u,23u},
    {83u,10u,24u}, {75u,10u,25u}, {68u,10u,26u}, {119u,11u,27u}, {201u,12u,28u}, {107u,11u,29u}, {207u,12u,30u}, {9u,8u,31u},
    {15u,6u,32u}, {13u,6u,33u}, {23u,7u,34u}, {38u,8u,35u}, {67u,9u,36u}, {58u,9u,37u}, {103u,10u,38u}, {90u,10u,39u},
    {161u,11u,40u}, {72u,10u,41u}, {127u,11u,42u}, {117u,11u,43u}, {110u,11u,44u}, {209u,12u,45u}, {206u,12u,46u}, {16u,9u,47u},
    {45u,8u,48u}, {21u,7u,49u}, {39u,8u,50u}, {69u,9u,51u}, {64u,9u,52u}, {114u,10u,53u}, {99u,10u,54u}, {87u,10u,55u},
    {158u,11u,56u}, {140u,11u,57u}, {252u,12u,58u}, {212u,12u,59u}, {199u,12u,60u}, {387u,13u,61u}, {365u,13u,62u}, {26u,10u,63u},
    {75u,9u,64u}, {36u,8u,65u}, {68u,9u,66u}, {65u,9u,67u}, {115u,10u,68u}, {101u,10u,69u}, {179u,11u,70u}, {164u,11u,71u},
    {155u,11u,72u}, {264u,12u,73u}, {246u,12u,74u}, {226u,12u,75u}, {395u,13u,76u}, {382u,13u,77u}, {362u,13u,78u}, {9u,9u,79u},
    {66u,9u,80u}, {30u,8u,81u}, {59u,9u,82u}, {56u,9u,83u}, {102u,10u,84u}, {185u,11u,85u}, {173u,11u,86u}, {265u,12u,87u},
    {142u,11u,88u}, {253u,12u,89u}, {232u,12u,90u}, {400u,13u,91u}, {388u,13u,92u}, {378u,13u,93u}, {445u,14u,94u}, {16u,10u,95u},
    {111u,10u,96u}, {54u,9u,97u}, {52u,9u,98u}, {100u,10u,99u}, {184u,11u,100u}, {178u,11u,101u}, {160u,11u,102u}, {133u,11u,103u},
    {257u,12u,104u}, {244u,12u,105u}, {228u,12u,106u}, {217u,12u,107u}, {385u,13u,108u}, {366u,13u,109u}, {715u,14u,110u}, {10u,10u,111u},
    {98u,10u,112u}, {48u,9u,113u}, {91u,10u,114u}, {88u,10u,115u}, {165u,11u,116u}, {157u,11u,117u}, {148u,11u,118u}, {261u,12u,119u},
    {248u,12u,120u}, {407u,13u,121u}, {397u,13u,122u}, {372u,13u,123u}, {380u,13u,124u}, {889u,15u,125u}, {884u,15u,126u}, {8u,10u,127u},
    {85u,10u,128u}, {84u,10u,129u}, {81u,10u,130u}, {159u,11u,131u}, {156u,11u,132u}, {143u,11u,133u}, {260u,12u,134u}, {249u,12u,135u},
    {427u,13u,136u}, {401u,13u,137u}, {392u,13u,138u}, {383u,13u,139u}, {727u,14u,140u}, {713u,14u,141u}, {708u,14u,142u}, {7u,10u,143u},
    {154u,11u,144u}, {76u,10u,145u}, {73u,10u,146u}, {141u,11u,147u}, {131u,11u,148u}, {256u,12u,149u}, {245u,12u,150u}, {426u,13u,151u},
    {406u,13u,152u}, {394u,13u,153u}, {384u,13u,154u}, {735u,14u,155u}, {359u,13u,156u}, {710u,14u,157u}, {352u,13u,158u}, {11u,11u,159u},
    {139u,11u,160u}, {129u,11u,161u}, {67u,10u,162u}, {125u,11u,163u}, {247u,12u,164u}, {233u,12u,165u}, {229u,12u,166u}, {219u,12u,167u},
    {393u,13u,168u}, {743u,14u,169u}, {737u,14u,170u}, {720u,14u,171u}, {885u,15u,172u}, {882u,15u,173u}, {439u,14u,174u}, {4u,10u,175u},
    {243u,12u,176u}, {120u,11u,177u}, {118u,11u,178u}, {115u,11u,179u}, {227u,12u,180u}, {223u,12u,181u}, {396u,13u,182u}, {746u,14u,183u},
    {742u,14u,184u}, {736u,14u,185u}, {721u,14u,186u}, {712u,14u,187u}, {706u,14u,188u}, {223u,13u,189u}, {436u,14u,190u}, {6u,11u,191u},
    {202u,12u,192u}, {224u,12u,193u}, {222u,12u,194u}, {218u,12u,195u}, {216u,12u,196u}, {389u,13u,197u}, {386u,13u,198u}, {381u,13u,199u},
    {364u,13u,200u}, {888u,15u,201u}, {443u,14u,202u}, {707u,14u,203u}, {440u,14u,204u}, {437u,14u,205u}, {1728u,16u,206u}, {4u,11u,207u},
    {747u,14u,208u}, {211u,12u,209u}, {210u,12u,210u}, {208u,12u,211u}, {370u,13u,212u}, {379u,13u,213u}, {734u,14u,214u}, {723u,14u,215u},
    {714u,14u,216u}, {1735u,16u,217u}, {883u,15u,218u}, {877u,15u,219u}, {876u,15u,220u}, {3459u,17u,221u}, {865u,15u,222u}, {2u,11u,223u},
    {377u,13u,224u}, {369u,13u,225u}, {102u,11u,226u}, {187u,12u,227u}, {726u,14u,228u}, {722u,14u,229u}, {358u,13u,230u}, {711u,14u,231u},
    {709u,14u,232u}, {866u,15u,233u}, {1734u,16u,234u}, {871u,15u,235u}, {3458u,17u,236u}, {870u,15u,237u}, {434u,14u,238u}, {0u,11u,239u},
    {12u,9u,240u}, {10u,8u,241u}, {7u,8u,242u}, {11u,9u,243u}, {10u,9u,244u}, {17u,10u,245u}, {11u,10u,246u}, {9u,10u,247u},
    {13u,11u,248u}, {12u,11u,249u}, {10u,11u,250u}, {7u,11u,251u}, {5u,11u,252u}, {3u,11u,253u}, {1u,11u,254u}, {3u,8u,255u},
};
static const Mp3Code codes_24[256] = {
    {15u,4u,0u}, {13u,4u,1u}, {46u,6u,2u}, {80u,7u,3u}, {146u,8u,4u}, {262u,9u,5u}, {248u,9u,6u}, {434u,10u,7u},
    {426u,10u,8u}, {669u,11u,9u}, {653u,11u,10u}, {649u,11u,11u}, {621u,11u,12u}, {517u,11u,13u}, {1032u,12u,14u}, {88u,9u,15u},
    {14u,4u,16u}, {12u,4u,17u}, {21u,5u,18u}, {38u,6u,19u}, {71u,7u,20u}, {130u,8u,21u}, {122u,8u,22u}, {216u,9u,23u},
    {209u,9u,24u}, {198u,9u,25u}, {327u,10u,26u}, {345u,10u,27u}, {319u,10u,28u}, {297u,10u,29u}, {279u,10u,30u}, {42u,8u,31u},
    {47u,6u,32u}, {22u,5u,33u}, {41u,6u,34u}, {74u,7u,35u}, {68u,7u,36u}, {128u,8u,37u}, {120u,8u,38u}, {221u,9u,39u},
    {207u,9u,40u}, {194u,9u,41u}, {182u,9u,42u}, {340u,10u,43u}, {315u,10u,44u}, {295u,10u,45u}, {541u,11u,46u}, {18u,7u,47u},
    {81u,7u,48u}, {39u,6u,49u}, {75u,7u,50u}, {70u,7u,51u}, {134u,8u,52u}, {125u,8u,53u}, {116u,8u,54u}, {220u,9u,55u},
    {204u,9u,56u}, {190u,9u,57u}, {178u,9u,58u}, {325u,10u,59u}, {311u,10u,60u}, {293u,10u,61u}, {271u,10u,62u}, {16u,7u,63u},
    {147u,8u,64u}, {72u,7u,65u}, {69u,7u,66u}, {135u,8u,67u}, {127u,8u,68u}, {118u,8u,69u}, {112u,8u,70u}, {210u,9u,71u},
    {200u,9u,72u}, {188u,9u,73u}, {352u,10u,74u}, {323u,10u,75u}, {306u,10u,76u}, {285u,10u,77u}, {540u,11u,78u}, {14u,7u,79u},
    {263u,9u,80u}, {66u,7u,81u}, {129u,8u,82u}, {126u,8u,83u}, {119u,8u,84u}, {114u,8u,85u}, {214u,9u,86u}, {202u,9u,87u},
    {192u,9u,88u}, {180u,9u,89u}, {341u,10u,90u}, {317u,10u,91u}, {301u,10u,92u}, {281u,10u,93u}, {262u,10u,94u}, {12u,7u,95u},
    {249u,9u,96u}, {123u,8u,97u}, {121u,8u,98u}, {117u,8u,99u}, {113u,8u,100u}, {215u,9u,101u}, {206u,9u,102u}, {195u,9u,103u},
    {185u,9u,104u}, {347u,10u,105u}, {330u,10u,106u}, {308u,10u,107u}, {291u,10u,108u}, {272u,10u,109u}, {520u,11u,110u}, {10u,7u,111u},
    {435u,10u,112u}, {115u,8u,113u}, {111u,8u,114u}, {109u,8u,115u}, {211u,9u,116u}, {203u,9u,117u}, {196u,9u,118u}, {187u,9u,119u},
    {353u,10u,120u}, {332u,10u,121u}, {313u,10u,122u}, {298u,10u,123u}, {283u,10u,124u}, {531u,11u,125u}, {381u,11u,126u}, {17u,8u,127u},
    {427u,10u,128u}, {212u,9u,129u}, {208u,9u,130u}, {205u,9u,131u}, {201u,9u,132u}, {193u,9u,133u}, {186u,9u,134u}, {177u,9u,135u},
    {169u,9u,136u}, {320u,10u,137u}, {303u,10u,138u}, {286u,10u,139u}, {268u,10u,140u}, {514u,11u,141u}, {377u,11u,142u}, {16u,8u,143u},
    {335u,10u,144u}, {199u,9u,145u}, {197u,9u,146u}, {191u,9u,147u}, {189u,9u,148u}, {181u,9u,149u}, {174u,9u,150u}, {333u,10u,151u},
    {321u,10u,152u}, {305u,10u,153u}, {289u,10u,154u}, {275u,10u,155u}, {521u,11u,156u}, {379u,11u,157u}, {371u,11u,158u}, {11u,8u,159u},
    {668u,11u,160u}, {184u,9u,161u}, {183u,9u,162u}, {179u,9u,163u}, {175u,9u,164u}, {344u,10u,165u}, {331u,10u,166u}, {314u,10u,167u},
    {304u,10u,168u}, {290u,10u,169u}, {277u,10u,170u}, {530u,11u,171u}, {383u,11u,172u}, {373u,11u,173u}, {366u,11u,174u}, {10u,8u,175u},
    {652u,11u,176u}, {346u,10u,177u}, {171u,9u,178u}, {168u,9u,179u}, {164u,9u,180u}, {318u,10u,181u}, {309u,10u,182u}, {299u,10u,183u},
    {287u,10u,184u}, {276u,10u,185u}, {263u,10u,186u}, {513u,11u,187u}, {375u,11u,188u}, {368u,11u,189u}, {362u,11u,190u}, {6u,8u,191u},
    {648u,11u,192u}, {322u,10u,193u}, {316u,10u,194u}, {312u,10u,195u}, {307u,10u,196u}, {302u,10u,197u}, {292u,10u,198u}, {284u,10u,199u},
    {269u,10u,200u}, {261u,10u,201u}, {512u,11u,202u}, {376u,11u,203u}, {370u,11u,204u}, {364u,11u,205u}, {359u,11u,206u}, {4u,8u,207u},
    {620u,11u,208u}, {300u,10u,209u}, {296u,10u,210u}, {294u,10u,211u}, {288u,10u,212u}, {282u,10u,213u}, {273u,10u,214u}, {266u,10u,215u},
    {515u,11u,216u}, {380u,11u,217u}, {374u,11u,218u}, {369u,11u,219u}, {365u,11u,220u}, {361u,11u,221u}, {357u,11u,222u}, {2u,8u,223u},
    {1033u,12u,224u}, {280u,10u,225u}, {278u,10u,226u}, {274u,10u,227u}, {267u,10u,228u}, {264u,10u,229u}, {259u,10u,230u}, {382u,11u,231u},
    {378u,11u,232u}, {372u,11u,233u}, {367u,11u,234u}, {363u,11u,235u}, {360u,11u,236u}, {358u,11u,237u}, {356u,11u,238u}, {0u,8u,239u},
    {43u,8u,240u}, {20u,7u,241u}, {19u,7u,242u}, {17u,7u,243u}, {15u,7u,244u}, {13u,7u,245u}, {11u,7u,246u}, {9u,7u,247u},
    {7u,7u,248u}, {6u,7u,249u}, {4u,7u,250u}, {7u,8u,251u}, {5u,8u,252u}, {3u,8u,253u}, {1u,8u,254u}, {3u,4u,255u},
};
static const int32_t synthesis_window[512] = {
    0, -1, -1, -1, -1, -1, -1, -2,
    -2, -2, -2, -3, -3, -4, -4, -5,
    -5, -6, -7, -7, -8, -9, -10, -11,
    -13, -14, -16, -17, -19, -21, -24, -26,
    -29, -31, -35, -38, -41, -45, -49, -53,
    -58, -63, -68, -73, -79, -85, -91, -97,
    -104, -111, -117, -125, -132, -139, -147, -154,
    -161, -169, -176, -183, -190, -196, -202, -208,
    213, 218, 222, 225, 227, 228, 228, 227,
    224, 221, 215, 208, 200, 189, 177, 163,
    146, 127, 106, 83, 57, 29, -2, -36,
    -72, -111, -153, -197, -244, -294, -347, -401,
    -459, -519, -581, -645, -711, -779, -848, -919,
    -991, -1064, -1137, -1210, -1283, -1356, -1428, -1498,
    -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962,
    -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063,
    2037, 2000, 1952, 1893, 1822, 1739, 1644, 1535,
    1414, 1280, 1131, 970, 794, 605, 402, 185,
    -45, -288, -545, -814, -1095, -1388, -1692, -2006,
    -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
    -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597,
    -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585,
    -9727, -9838, -9916, -9959, -9966, -9935, -9863, -9750,
    -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
    6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082,
    70, -998, -2122, -3300, -4533, -5818, -7154, -8540,
    -9975, -11455, -12980, -14548, -16155, -17799, -19478, -21189,
    -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
    -37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137,
    -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420,
    -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
    75038, 74992, 74856, 74630, 74313, 73908, 73415, 72835,
    72169, 71420, 70590, 69679, 68692, 67629, 66494, 65290,
    64019, 62684, 61289, 59838, 58333, 56778, 55178, 53534,
    51853, 50137, 48390, 46617, 44821, 43006, 41176, 39336,
    37489, 35640, 33791, 31947, 30112, 28289, 26482, 24694,
    22929, 21189, 19478, 17799, 16155, 14548, 12980, 11455,
    9975, 8540, 7154, 5818, 4533, 3300, 2122, 998,
    -70, -1082, -2037, -2935, -3776, -4561, -5288, -5959,
    6574, 7134, 7640, 8092, 8492, 8840, 9139, 9389,
    9592, 9750, 9863, 9935, 9966, 9959, 9916, 9838,
    9727, 9585, 9416, 9219, 8998, 8755, 8491, 8209,
    7910, 7597, 7271, 6935, 6589, 6237, 5879, 5517,
    5153, 4788, 4425, 4063, 3705, 3351, 3004, 2663,
    2330, 2006, 1692, 1388, 1095, 814, 545, 288,
    45, -185, -402, -605, -794, -970, -1131, -1280,
    -1414, -1535, -1644, -1739, -1822, -1893, -1952, -2000,
    2037, 2063, 2080, 2087, 2085, 2075, 2057, 2032,
    2001, 1962, 1919, 1870, 1817, 1759, 1698, 1634,
    1567, 1498, 1428, 1356, 1283, 1210, 1137, 1064,
    991, 919, 848, 779, 711, 645, 581, 519,
    459, 401, 347, 294, 244, 197, 153, 111,
    72, 36, 2, -29, -57, -83, -106, -127,
    -146, -163, -177, -189, -200, -208, -215, -221,
    -224, -227, -228, -228, -227, -225, -222, -218,
    213, 208, 202, 196, 190, 183, 176, 169,
    161, 154, 147, 139, 132, 125, 117, 111,
    104, 97, 91, 85, 79, 73, 68, 63,
    58, 53, 49, 45, 41, 38, 35, 31,
    29, 26, 24, 21, 19, 17, 16, 14,
    13, 11, 10, 9, 8, 7, 7, 6,
    5, 5, 4, 4, 3, 3, 2, 2,
    2, 2, 1, 1, 1, 1, 1, 1,
};
#define MP3_PI 3.14159265358979323846
#define MP3_FRAME_BYTES 1441u
#define MP3_RESERVOIR_BYTES 511u
#define MP3_TREE_NODES 3000u

typedef struct { const uint8_t *data; size_t pos, limit; bool bad; } Mp3Bits;
typedef struct { int16_t next[2]; } Mp3Node;
typedef struct {
    unsigned length, big, gain, compress, block, mixed;
    unsigned table[3], region[2], subgain[3], pre, scale, count;
    uint8_t sf_l[22], sf_s[13][3];
} Mp3Granule;
struct Psvr2Mp3Decoder {
    FILE *file;
    unsigned rate, channels, rate_index;
    uint64_t frames, expected_frames, remaining;
    size_t skip;
    bool metadata_frame, gapless;
    int failure;
    bool eof;
    uint8_t reservoir[MP3_RESERVOIR_BYTES];
    size_t reservoir_length;
    int16_t pcm[1152 * 2];
    size_t pcm_pos, pcm_count;
    Mp3Node nodes[MP3_TREE_NODES];
    unsigned node_count;
    int16_t roots[32];
    float power[8207], mdct_long[36][18], mdct_short[12][6];
    float synth_cos[64][32], window[4][36], alias_cs[8], alias_ca[8];
    float overlap[2][32][18], synthesis[2][1024];
};

static const uint16_t bands_long[3][23] = {
    {0,4,8,12,16,20,24,30,36,44,52,62,74,90,110,134,162,196,238,288,342,418,576},
    {0,4,8,12,16,20,24,30,36,42,50,60,72,88,106,128,156,190,230,276,330,384,576},
    {0,4,8,12,16,20,24,30,36,44,54,66,82,102,126,156,194,240,296,364,448,550,576}
};
static const uint16_t bands_short[3][14] = {
    {0,4,8,12,16,22,30,40,52,66,84,106,136,192},
    {0,4,8,12,16,22,28,38,50,64,80,100,126,192},
    {0,4,8,12,16,22,30,42,58,78,104,138,180,192}
};
static const uint8_t pretab[22] = {0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2,0};
static const uint8_t slen[16][2] = {
    {0,0},{0,1},{0,2},{0,3},{3,0},{1,1},{1,2},{1,3},
    {2,1},{2,2},{2,3},{3,1},{3,2},{3,3},{4,2},{4,3}
};
static const uint8_t linbits[32] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4,6,8,10,13,4,5,6,7,8,9,11,13
};
static const Mp3Code count_codes[16] = {
    {1,1,0},{5,4,1},{4,4,2},{5,5,3},{6,4,4},{5,6,5},{4,5,6},{4,6,7},
    {7,4,8},{3,5,9},{6,5,10},{0,6,11},{7,5,12},{2,6,13},{3,6,14},{1,6,15}
};
static unsigned bits(Mp3Bits *b, unsigned count) {
    if (count > 24 || b->pos > b->limit || count > b->limit - b->pos) {
        b->bad = true; return 0;
    }
    unsigned value = 0;
    for (unsigned i=0; i<count; ++i, ++b->pos)
        value = value * 2u + ((b->data[b->pos / 8] >> (7u - (unsigned)(b->pos % 8))) & 1u);
    return value;
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static bool make_tree(Psvr2Mp3Decoder *d, unsigned table,
                      const Mp3Code *codes, size_t count) {
    if (d->node_count >= MP3_TREE_NODES) return false;
    unsigned root = d->node_count++;
    d->roots[table] = (int16_t)root;
    d->nodes[root].next[0] = d->nodes[root].next[1] = INT16_MIN;
    for (size_t i=0; i<count; ++i) {
        unsigned node=root;
        for (unsigned k=codes[i].bits; k>0; --k) {
            unsigned branch=(codes[i].code>>(k-1u))&1u;
            int16_t *child=&d->nodes[node].next[branch];
            if (k==1) { if (*child!=INT16_MIN) return false;
                *child=(int16_t)(-(int)codes[i].value-1); }
            else {
                if (*child==INT16_MIN) {
                    if (d->node_count>=MP3_TREE_NODES) return false;
                    *child=(int16_t)d->node_count++;
                    d->nodes[(unsigned)*child].next[0]=INT16_MIN;
                    d->nodes[(unsigned)*child].next[1]=INT16_MIN;
                }
                if (*child<0) return false;
                node=(unsigned)*child;
            }
        }
    }
    return true;
}
static bool initialize(Psvr2Mp3Decoder *d) {
    for (unsigned i=0;i<32;++i) d->roots[i]=INT16_MIN;
#define TREE(n) if (!make_tree(d,n,codes_##n,sizeof(codes_##n)/sizeof(codes_##n[0]))) return false
    TREE(1);TREE(2);TREE(3);TREE(5);TREE(6);TREE(7);TREE(8);TREE(9);
    TREE(10);TREE(11);TREE(12);TREE(13);TREE(15);TREE(16);TREE(24);
#undef TREE
    for(unsigned i=17;i<24;++i)d->roots[i]=d->roots[16];
    for(unsigned i=25;i<32;++i)d->roots[i]=d->roots[24];
    for(unsigned i=0;i<8207;++i)d->power[i]=powf((float)i,4.0f/3.0f);
    for(unsigned n=0;n<36;++n) {
        for(unsigned k=0;k<18;++k)
            d->mdct_long[n][k]=(float)cos(MP3_PI/72.0*(2.0*n+19.0)*(2.0*k+1.0));
        d->window[0][n]=(float)sin(MP3_PI/36.0*(n+0.5));
        d->window[1][n]=n<18?d->window[0][n]:n<24?1.0f:n<30?(float)sin(MP3_PI/12.0*(n-18.0+0.5)):0;
        d->window[3][n]=n<6?0:n<12?(float)sin(MP3_PI/12.0*(n-6.0+0.5)):n<18?1.0f:d->window[0][n];
    }
    for(unsigned n=0;n<12;++n) {
        d->window[2][n]=(float)sin(MP3_PI/12.0*(n+0.5));
        for(unsigned k=0;k<6;++k)
            d->mdct_short[n][k]=(float)cos(MP3_PI/24.0*(2.0*n+7.0)*(2.0*k+1.0));
    }
    for(unsigned n=0;n<64;++n)for(unsigned k=0;k<32;++k)
        d->synth_cos[n][k]=(float)cos(MP3_PI/64.0*(16.0+n)*(2.0*k+1.0));
    const float ci[8]={-.6f,-.535f,-.33f,-.185f,-.095f,-.041f,-.0142f,-.0037f};
    for(unsigned i=0;i<8;++i) {
        d->alias_cs[i]=1.0f/sqrtf(1.0f+ci[i]*ci[i]);
        d->alias_ca[i]=ci[i]*d->alias_cs[i];
    }
    return true;
}
static int symbol(Psvr2Mp3Decoder *d, Mp3Bits *b, unsigned table) {
    if(table==0)return 0;
    if(table>=32 || d->roots[table]==INT16_MIN){b->bad=true;return 0;}
    int node=d->roots[table];
    for(unsigned depth=0;depth<19;++depth) {
        node=d->nodes[node].next[bits(b,1)];
        if(b->bad || node==INT16_MIN){b->bad=true;return 0;}
        if(node<0)return -node-1;
    }
    b->bad=true;return 0;
}
static unsigned count_symbol(Mp3Bits *b, unsigned table) {
    if(table)return 15u-bits(b,4);
    unsigned code=0;
    for(unsigned length=1;length<=6;++length) {
        code=2u*code+bits(b,1);
        if(b->bad)return 0;
        for(unsigned i=0;i<16;++i)
            if(count_codes[i].bits==length && count_codes[i].code==code)return i;
    }
    b->bad=true;return 0;
}
static bool scalefactors(Mp3Bits *b, Mp3Granule *g,
                         const Mp3Granule *previous, unsigned scfsi) {
    unsigned a=slen[g->compress][0],c=slen[g->compress][1];
    if(g->block==2) {
        if(g->mixed)for(unsigned s=0;s<8;++s)g->sf_l[s]=(uint8_t)bits(b,a);
        for(unsigned s=g->mixed?3u:0u;s<12;++s)
            for(unsigned w=0;w<3;++w)g->sf_s[s][w]=(uint8_t)bits(b,s<6?a:c);
    } else {
        const unsigned split[5]={0,6,11,16,21};
        for(unsigned part=0;part<4;++part)
            for(unsigned s=split[part];s<split[part+1];++s)
                g->sf_l[s]=(previous && (scfsi&(8u>>part)))?previous->sf_l[s]:(uint8_t)bits(b,s<11?a:c);
    }
    return !b->bad;
}
static bool spectrum(Psvr2Mp3Decoder *d, Mp3Bits *b, Mp3Granule *g,
                      float output[576]) {
    int values[576]={0};
    unsigned region0=36,region1=576;
    const uint16_t *lb=bands_long[d->rate_index],*sb=bands_short[d->rate_index];
    if(!g->block) {
        unsigned r0=g->region[0]+1u,r1=r0+g->region[1]+1u;
        if(r0>22 || r1>22)return false;
        region0=lb[r0];region1=lb[r1];
    }
    unsigned used=0;
    for(;used<g->big*2u;used+=2) {
        unsigned table=g->table[used<region0?0:used<region1?1:2];
        int s=symbol(d,b,table);
        for(unsigned pair=0;pair<2;++pair) {
            unsigned value=pair?(unsigned)s&15u:(unsigned)s>>4;
            if(value==15u)value+=bits(b,linbits[table]);
            if(value>8206u || b->bad)return false;
            int signed_value=(int)value;
            if(value && bits(b,1))signed_value=-signed_value;
            values[used+pair]=signed_value;
        }
    }
    if(b->bad)return false;
    while(used+4u<=576 && b->pos<b->limit) {
        size_t before=b->pos;
        unsigned value=count_symbol(b,g->count);
        int quartet[4];
        for(unsigned i=0;i<4;++i) {
            int v=(int)((value>>(3u-i))&1u);
            if(v && bits(b,1))v=-v;
            quartet[i]=v;
        }
        /* The count1 code may cross the part boundary; those bits are stuffing,
         * not a decoded quartet. Never consume them in the following granule. */
        if(b->bad){b->bad=false;b->pos=before;break;}
        memcpy(values+used,quartet,sizeof(quartet));used+=4;
    }
    b->pos=b->limit;
    float step=(float)((int)g->gain-210)*0.25f;
    float sf_step=g->scale?1.0f:0.5f;
    memset(output,0,576*sizeof(*output));
    unsigned cursor=0;
    if(g->block!=2 || g->mixed) {
        unsigned limit=g->block==2?8u:22u;
        for(unsigned band=0;band<limit;++band) {
            float factor=exp2f(step-sf_step*(float)(g->sf_l[band]+g->pre*pretab[band]));
            for(;cursor<lb[band+1];++cursor) {
                int v=values[cursor];
                output[cursor]=(v<0?-factor:factor)*d->power[(unsigned)(v<0?-v:v)];
            }
        }
    }
    if(g->block==2) {
        for(unsigned band=g->mixed?3u:0u;band<13;++band)
            for(unsigned w=0;w<3;++w) {
                float factor=exp2f(step-2.0f*(float)g->subgain[w]-sf_step*(float)g->sf_s[band][w]);
                for(unsigned k=sb[band];k<sb[band+1];++k) {
                    int v=values[cursor++];
                    output[3u*k+w]=(v<0?-factor:factor)*d->power[(unsigned)(v<0?-v:v)];
                }
            }
    }
    return cursor==576;
}
static void stereo_band(float x[2][576], unsigned start, unsigned end,
                         unsigned stride, unsigned position, bool intensity,
                         bool middle_side) {
    bool use_intensity=intensity && position<7;
    float left=1,right=0;
    if(use_intensity) {
        if(position==0){left=0;right=1;}
        else if(position==6){left=1;right=0;}
        else {float ratio=tanf((float)(MP3_PI/12.0)*(float)position);
            left=ratio/(1.0f+ratio);right=1.0f/(1.0f+ratio);}
        /* Intensity bands use their tangent pan even when M/S is enabled. */
    }
    for(unsigned i=start;i<end;i+=stride) {
        float a=x[0][i],b=x[1][i];
        if(use_intensity){x[0][i]=a*left;x[1][i]=a*right;}
        else if(middle_side){x[0][i]=(a+b)*0.7071067811865475f;x[1][i]=(a-b)*0.7071067811865475f;}
    }
}
static void stereo(Psvr2Mp3Decoder *d, Mp3Granule *right,
                    float x[2][576], unsigned extension) {
    bool intensity=(extension&1u)!=0,ms=(extension&2u)!=0;
    const uint16_t *lb=bands_long[d->rate_index],*sb=bands_short[d->rate_index];
    if(right->block!=2) {
        int last=-1;
        for(unsigned band=0;band<22;++band)
            for(unsigned i=lb[band];i<lb[band+1];++i)if(x[1][i]!=0)last=(int)band;
        for(unsigned band=0;band<22;++band)
            stereo_band(x,lb[band],lb[band+1],1,right->sf_l[band==21?20:band],
                        intensity && (int)band>last,ms);
    } else {
        bool short_nonzero=false;
        for(unsigned w=0;w<3;++w) {
            int last=(int)(right->mixed?2:0)-1;
            for(unsigned band=right->mixed?3u:0u;band<13;++band)
                for(unsigned k=sb[band];k<sb[band+1];++k)
                    if(x[1][3u*k+w]!=0){last=(int)band;short_nonzero=true;}
            for(unsigned band=right->mixed?3u:0u;band<13;++band)
                stereo_band(x,3u*sb[band]+w,3u*sb[band+1]+w,3,
                            right->sf_s[band==12?11:band][w],intensity && (int)band>last,ms);
        }
        if(right->mixed) {
            int last=-1;
            for(unsigned band=0;band<8;++band)
                for(unsigned i=lb[band];i<lb[band+1];++i)if(x[1][i]!=0)last=(int)band;
            for(unsigned band=0;band<8;++band)
                stereo_band(x,lb[band],lb[band+1],1,right->sf_l[band],
                            intensity && !short_nonzero && (int)band>last,ms);
        }
    }
}
static bool synthesize(Psvr2Mp3Decoder *d, unsigned channel, unsigned granule,
                        Mp3Granule *g, float x[576]) {
    unsigned alias_limit=g->block==2?(g->mixed?2u:1u):32u;
    for(unsigned sb=1;sb<alias_limit;++sb)for(unsigned i=0;i<8;++i) {
        unsigned a=sb*18u-1u-i,b=sb*18u+i;
        float u=x[a],v=x[b];
        x[a]=u*d->alias_cs[i]-v*d->alias_ca[i];
        x[b]=v*d->alias_cs[i]+u*d->alias_ca[i];
    }
    float subband[32][18];
    for(unsigned sb=0;sb<32;++sb) {
        unsigned type=g->mixed && sb<2?0:g->block;
        float y[36]={0};
        if(type==2) {
            for(unsigned w=0;w<3;++w)for(unsigned n=0;n<12;++n) {
                float sum=0;
                for(unsigned k=0;k<6;++k)sum+=x[sb*18u+3u*k+w]*d->mdct_short[n][k];
                y[6u+6u*w+n]+=sum*d->window[2][n];
            }
        } else for(unsigned n=0;n<36;++n) {
            float sum=0;
            for(unsigned k=0;k<18;++k)sum+=x[sb*18u+k]*d->mdct_long[n][k];
            y[n]=sum*d->window[type][n];
        }
        for(unsigned n=0;n<18;++n) {
            float value=y[n]+d->overlap[channel][sb][n];
            subband[sb][n]=(sb&n&1u)?-value:value;
            d->overlap[channel][sb][n]=y[n+18u];
        }
    }
    float *history=d->synthesis[channel];
    for(unsigned time=0;time<18;++time) {
        memmove(history+64,history,960*sizeof(*history));
        for(unsigned i=0;i<64;++i) {
            float sum=0;
            for(unsigned sb=0;sb<32;++sb)sum+=d->synth_cos[i][sb]*subband[sb][time];
            history[i]=sum;
        }
        for(unsigned j=0;j<32;++j) {
            double sum=0;
            for(unsigned i=0;i<16;++i)
                sum+=(double)history[128u*(i/2u)+(i&1u?96u:0u)+j]*(double)synthesis_window[32u*i+j];
            /* D is stored as exact multiples of 1/65536; PCM is scaled 32768. */
            double value=sum*0.5;
            if(!isfinite(value))return false;
            int16_t pcm=value<=-32768?-32768:value>=32767?32767:(int16_t)lrint(value);
            d->pcm[(granule*576u+time*32u+j)*d->channels+channel]=pcm;
        }
    }
    return true;
}
static uint16_t crc_bits(uint16_t crc, const uint8_t *data, size_t count) {
    for(size_t i=0;i<count;++i) {
        unsigned bit=(data[i/8]>>(7u-(unsigned)(i%8)))&1u;
        unsigned carry=((crc>>15)&1u)^bit;
        crc=(uint16_t)((unsigned)crc<<1);
        if(carry)crc^=UINT16_C(0x8005);
    }
    return crc;
}
static bool parse_side(const uint8_t *data, unsigned channels,
                        Mp3Granule g[2][2], unsigned scfsi[2], unsigned *back) {
    Mp3Bits b={data,0,channels==1?136u:256u,false};
    *back=bits(&b,9);(void)bits(&b,channels==1?5:3);
    for(unsigned ch=0;ch<channels;++ch)scfsi[ch]=bits(&b,4);
    for(unsigned gr=0;gr<2;++gr)for(unsigned ch=0;ch<channels;++ch) {
        Mp3Granule *p=&g[gr][ch];
        p->length=bits(&b,12);p->big=bits(&b,9);p->gain=bits(&b,8);p->compress=bits(&b,4);
        bool switched=bits(&b,1)!=0;
        if(switched) {
            p->block=bits(&b,2);p->mixed=bits(&b,1);
            p->table[0]=bits(&b,5);p->table[1]=bits(&b,5);
            for(unsigned w=0;w<3;++w)p->subgain[w]=bits(&b,3);
            if(!p->block || (p->mixed && p->block!=2))return false;
        } else {
            for(unsigned i=0;i<3;++i)p->table[i]=bits(&b,5);
            p->region[0]=bits(&b,4);p->region[1]=bits(&b,3);
            if(p->region[0]+p->region[1]>20)return false;
        }
        p->pre=bits(&b,1);p->scale=bits(&b,1);p->count=bits(&b,1);
        if(p->big>288)return false;
        for(unsigned i=0;i<3;++i)if(p->table[i]==4 || p->table[i]==14)return false;
    }
    return !b.bad && b.pos==b.limit;
}
static bool xing(Psvr2Mp3Decoder *d, const uint8_t *data, size_t bytes) {
    if(bytes<8 || (memcmp(data,"Xing",4) && memcmp(data,"Info",4)))return true;
    d->metadata_frame=true;
    uint32_t flags=be32(data+4);size_t pos=8;
    if(flags&~UINT32_C(15))return false;
    if(flags&1u){if(bytes-pos<4)return false;d->expected_frames=be32(data+pos);pos+=4;}
    if(flags&2u){if(bytes-pos<4)return false;pos+=4;}
    if(flags&4u){if(bytes-pos<100)return false;pos+=100;}
    if(flags&8u){if(bytes-pos<4)return false;pos+=4;}
    if(bytes-pos>=24 && (!memcmp(data+pos,"LAME",4) || !memcmp(data+pos,"Lavc",4))) {
        unsigned delay=((unsigned)data[pos+21]<<4)|(data[pos+22]>>4);
        unsigned padding=((unsigned)(data[pos+22]&15u)<<8)|data[pos+23];
        uint64_t total=d->expected_frames*1152u;
        if(!d->expected_frames || total<(uint64_t)delay+padding || padding<529)return false;
        d->gapless=true;d->skip=(size_t)delay+529u;
        d->remaining=total-delay-padding;
    }
    return true;
}
static int finish_stream(Psvr2Mp3Decoder *d) {
    if(d->metadata_frame && d->expected_frames && d->frames-1u!=d->expected_frames)return EINVAL;
    if(d->gapless && (d->skip || d->remaining))return EINVAL;
    d->eof=true;return 0;
}
static int decode_frame(Psvr2Mp3Decoder *d) {
    uint8_t frame[MP3_FRAME_BYTES],main_data[MP3_RESERVOIR_BYTES+MP3_FRAME_BYTES];
    size_t got=fread(frame,1,4,d->file);
    if(!got){if(ferror(d->file))return EIO;return finish_stream(d);}
    if(got!=4)return EINVAL;
    if(!memcmp(frame,"TAG",3)) {
        uint8_t tag[124];
        if(fread(tag,1,sizeof(tag),d->file)!=sizeof(tag) || fgetc(d->file)!=EOF || ferror(d->file))return EINVAL;
        return finish_stream(d);
    }
    uint32_t h=be32(frame);
    if((h&UINT32_C(0xffe00000))!=UINT32_C(0xffe00000))return EINVAL;
    if(((h>>19)&3u)!=3 || ((h>>17)&3u)!=1)return ENOTSUP;
    unsigned bitrate=(h>>12)&15u,rate_index=(h>>10)&3u,mode=(h>>6)&3u;
    static const unsigned rates[3]={44100,48000,32000};
    static const unsigned br[15]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    if(!bitrate)return ENOTSUP;
    if(bitrate==15 || rate_index==3 || (h&3u)==2u)return EINVAL;
    if(h&3u)return ENOTSUP; /* De-emphasis is not silently omitted. */
    unsigned channels=mode==3?1u:2u,rate=rates[rate_index];
    if(d->rate && (d->rate!=rate || d->channels!=channels))return ENOTSUP;
    d->rate=rate;d->channels=channels;d->rate_index=rate_index;
    unsigned length=144000u*br[bitrate]/rate+((h>>9)&1u);
    unsigned crc=(h&(1u<<16))?0u:2u,side=channels==1?17u:32u;
    unsigned offset=4u+crc+side;
    if(length>sizeof(frame) || length<offset)return EINVAL;
    if(fread(frame+4,1,length-4u,d->file)!=length-4u)return ferror(d->file)?EIO:EINVAL;
    if(crc) {
        uint16_t sum=crc_bits(UINT16_C(0xffff),frame+2,16);
        sum=crc_bits(sum,frame+6,side*8u);
        if(sum!=(uint16_t)(((unsigned)frame[4]<<8)|frame[5]))return EINVAL;
    }
    Mp3Granule g[2][2]={0};unsigned scfsi[2]={0},back=0;
    if(!parse_side(frame+4u+crc,channels,g,scfsi,&back) || back>d->reservoir_length)return EINVAL;
    size_t payload=length-offset,total=(size_t)back+payload;
    if(!d->frames) {
        bool empty=true;
        for(unsigned gr=0;gr<2;++gr)for(unsigned ch=0;ch<channels;++ch)
            if(g[gr][ch].length || g[gr][ch].big)empty=false;
        if(empty && !xing(d,frame+offset,payload))return EINVAL;
    }
    if(d->metadata_frame && d->expected_frames && d->frames>d->expected_frames)return EINVAL;
    memcpy(main_data,d->reservoir+d->reservoir_length-back,back);
    memcpy(main_data+back,frame+offset,payload);
    Mp3Bits stream={main_data,0,total*8u,false};
    for(unsigned gr=0;gr<2;++gr) {
        float x[2][576]={{0}};
        for(unsigned ch=0;ch<channels;++ch) {
            Mp3Granule *p=&g[gr][ch];
            if(p->length>stream.limit-stream.pos)return EINVAL;
            Mp3Bits part={main_data,stream.pos,stream.pos+p->length,false};
            if(!scalefactors(&part,p,gr?&g[0][ch]:NULL,scfsi[ch]) || !spectrum(d,&part,p,x[ch]))return EINVAL;
            stream.pos+=p->length;
        }
        if(channels==2 && mode==1)stereo(d,&g[gr][1],x,(h>>4)&3u);
        for(unsigned ch=0;ch<channels;++ch)if(!synthesize(d,ch,gr,&g[gr][ch],x[ch]))return EINVAL;
    }
    /* Retain the physical main-data tail, including ancillary bytes. The next
     * frame's main_data_begin is a byte offset into that physical stream. */
    size_t keep=payload<MP3_RESERVOIR_BYTES?payload:MP3_RESERVOIR_BYTES;
    size_t old=MP3_RESERVOIR_BYTES-keep;
    if(old>d->reservoir_length)old=d->reservoir_length;
    memmove(d->reservoir,d->reservoir+d->reservoir_length-old,old);
    memcpy(d->reservoir+old,frame+length-keep,keep);
    d->reservoir_length=old+keep;
    ++d->frames;d->pcm_pos=0;d->pcm_count=1152;
    if(d->metadata_frame && d->frames==1)d->pcm_count=0;
    else if(d->gapless) {
        size_t skip=d->skip<1152?d->skip:1152;
        d->skip-=skip;d->pcm_pos=skip;
        size_t available=1152-skip;
        if(d->remaining<available)available=(size_t)d->remaining;
        d->pcm_count=skip+available;d->remaining-=available;
    }
    return 0;
}
static bool skip_id3(FILE *file) {
    uint8_t header[10];
    size_t n=fread(header,1,sizeof(header),file);
    if(n<3 || memcmp(header,"ID3",3))return fseek(file,0,SEEK_SET)==0;
    if(n!=10 || header[3]<2 || header[3]>4 || header[4]==255)return false;
    unsigned size=0;
    for(unsigned i=6;i<10;++i){if(header[i]&128u)return false;size=(size<<7)|header[i];}
    if(size>16u*1024u*1024u)return false;
    if(header[3]==4 && (header[5]&16u))size+=10;
    return fseek(file,(long)size,SEEK_CUR)==0;
}
Psvr2Mp3Decoder *psvr2_mp3_open(const char *path, char *error, size_t error_cap) {
    int failure=EINVAL;
    Psvr2Mp3Decoder *d=NULL;
    if(!path)goto fail;
    d=calloc(1,sizeof(*d));if(!d){failure=ENOMEM;goto fail;}
    d->file=fopen(path,"rb");if(!d->file){failure=errno;goto fail;}
    if(!initialize(d)){failure=EOVERFLOW;goto fail;}
    if(!skip_id3(d->file))goto fail;
    failure=decode_frame(d);
    if(failure || d->eof){if(!failure)failure=EINVAL;goto fail;}
    if(error && error_cap)error[0]='\0';
    return d;
fail:
    if(error && error_cap)snprintf(error,error_cap,"MP3: %s",strerror(failure));
    psvr2_mp3_close(d);errno=failure;return NULL;
}
unsigned psvr2_mp3_sample_rate(const Psvr2Mp3Decoder *d){return d?d->rate:0;}
unsigned psvr2_mp3_channels(const Psvr2Mp3Decoder *d){return d?d->channels:0;}
int64_t psvr2_mp3_read_frames(Psvr2Mp3Decoder *d,int16_t *pcm,size_t max_frames) {
    if(!d || (!pcm && max_frames) || max_frames>(size_t)INT64_MAX ||
       max_frames>SIZE_MAX/(2u*sizeof(*pcm))){errno=EINVAL;return -1;}
    if(d->failure){errno=d->failure;return -1;}
    size_t written=0;
    while(written<max_frames) {
        if(d->pcm_pos==d->pcm_count) {
            if(d->eof)break;
            int failure=decode_frame(d);
            if(failure){d->failure=failure;errno=failure;return written?(int64_t)written:-1;}
            if(d->eof)break;
            if(d->pcm_pos==d->pcm_count)continue;
        }
        size_t n=d->pcm_count-d->pcm_pos;
        if(n>max_frames-written)n=max_frames-written;
        memcpy(pcm+written*d->channels,d->pcm+d->pcm_pos*d->channels,n*d->channels*sizeof(*pcm));
        written+=n;d->pcm_pos+=n;
    }
    return (int64_t)written;
}
void psvr2_mp3_close(Psvr2Mp3Decoder *d){if(d){if(d->file)fclose(d->file);free(d);}}
