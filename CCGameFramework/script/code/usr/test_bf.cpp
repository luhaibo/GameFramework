#include "/include/io"
#include "/include/shell"
#include "/include/writefile"
#include "/include/proc"
void run(char* path, char* txt) {
    char* cmd = malloc(256);
    put_string("[x] ");
    put_string(path);
    put_string(" => ");
    strcpy(cmd, "/usr/bf_");
    strcat(cmd, path);
    writefile(cmd, txt, strlen(txt), 1);
    strcpy(cmd, "bf /usr/bf_");
    strcat(cmd, path);
    sleep(200);
    shell(cmd);
    free(cmd);
    newline();
}
int main(int argc, char** argv) {
    put_string("========== [#19 TEST BF] ==========\n");
    run("hello", "++++++++++[>+>+++>+++++++>++++++++++<<<<-]>>>++.>+.+++++++..+++.<<++.>+++++++++++++++.>.+++.------.--------.<<+.<.");
    run("love", "++++++++ [ > +++++++++ < - ] > +.[-]++++ [ > ++++++++ < - ] >.++++++[>++<-]>.+++.+++++++.-----------------.[-]++++ [ > ++++++++ < - ] >. -- [ >+++ < - ] >-.----------.++++++.");
    run("prime", ">++++++++[<++++++++>-]<++++++++++++++++.[-]>++++++++++[<++++++++++>-]<++++++++++++++.[-]>++++++++++[<++++++++++>-]<+++++.[-]>++++++++++[<++++++++++>-]<+++++++++.[-]>++++++++++[<++++++++++>-]<+.[-]>++++++++++[<++++++++++>-]<+++++++++++++++.[-]>+++++[<+++++>-]<+++++++.[-]>++++++++++[<++++++++++>-]<+++++++++++++++++.[-]>++++++++++[<++++++++++>-]<++++++++++++.[-]>+++++[<+++++>-]<+++++++.[-]>++++++++++[<++++++++++>-]<++++++++++++++++.[-]>++++++++++[<++++++++++>-]<+++++++++++.[-]>+++++++[<+++++++>-]<+++++++++.[-]>+++++[<+++++>-]<+++++++.[-]+[->,----------[<+>-------------------------------------->[>+>+<<-]>>[<<+>>-]<>>>+++++++++[<<<[>+>+<<-]>>[<<+>>-]<[<<+>>-]>>-]<<<[-]<<[>+<-]]<]>>[<<+>>-]<<>+<-[>+[>+>+<<-]>>[<<+>>-]<>+<-->>>>>>>>+<<<<<<<<[>+<-<[>>>+>+<<<<-]>>>>[<<<<+>>>>-]<<<>[>>+>+<<<-]>>>[<<<+>>>-]<<<<>>>[>+>+<<-]>>[<<+>>-]<<<[>>>>>+<<<[>+>+<<-]>>[<<+>>-]<[>>[-]<<-]>>[<<<<[>+>+<<-]>>[<<+>>-]<>>>-]<<<-<<-]+>>[<<[-]>>-]<<>[-]<[>>>>>>[-]<<<<<<-]<<>>[-]>[-]<<<]>>>>>>>>[-<<<<<<<[-]<<[>>+>+<<<-]>>>[<<<+>>>-]<<<>>[>+<-]>[[>+>+<<-]>>[<<+>>-]<>+++++++++<[>>>+<<[>+>[-]<<-]>[<+>-]>[<<++++++++++>>-]<<-<-]+++++++++>[<->-]<[>+<-]<[>+<-]<[>+<-]>>>[<<<+>>>-]<>+++++++++<[>>>+<<[>+>[-]<<-]>[<+>-]>[<<++++++++++>>>+<-]<<-<-]>>>>[<<<<+>>>>-]<<<<>[-]<<+>]<[[>+<-]+++++++[<+++++++>-]<-><.[-]>>[<<+>>-]<<-]>++++[<++++++++>-]<.[-]>>>>>>>]<<<<<<<<>[-]<[-]<<-]++++++++++.[-]");
    put_string("========== [#19 TEST BF] ==========\n");
    return 0;
}