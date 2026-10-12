#include "startup.h"
#include <stddef.h>
uint64_t luxrt_startup_argc(const uint64_t *s){return s[0];}
char **luxrt_startup_argv(const uint64_t *s){return (char **)(s+1);}
char **luxrt_startup_env(const uint64_t *s){return (char **)(s+s[0]+2);}
uint64_t luxrt_startup_pagesize(const uint64_t *s){
 char **env=luxrt_startup_env(s);while(*env)env++;
 const uint64_t *aux=(const uint64_t *)(env+1);
 for(;aux[0];aux+=2)if(aux[0]==6)return aux[1];
 return 0;
}
const char *luxrt_getenv(char *const *env,const char *name){
 size_t n=0;for(;name[n];n++)if(name[n]=='=')return NULL;
 if(!n)return NULL;
 for(;*env;env++){
  size_t i=0;for(;i<n;i++)if((*env)[i]!=name[i])break;
  if(i==n&&(*env)[i]=='=')return *env+n+1;
 }
 return NULL;
}
