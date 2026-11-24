#include "../../Common/UsnJournalGuard.h"
#include <cassert>
#include <iostream>
using namespace fsyn;
int main(){
 UsnState st{111,100,150,400};
 assert(!IsUsnCursorUsable(st,{112,200}));
 assert(!IsUsnCursorUsable(st,{111,149}));
 assert(!IsUsnCursorUsable(st,{111,401}));
 assert(IsUsnCursorUsable(st,{111,150}));
 assert(IsUsnCursorUsable(st,{111,400}));
 assert(!IsUsnCursorUsable({111,200,450,400},{111,400}));
 std::cout<<"PASS: USN journal identity and validity bounds\n";
}
