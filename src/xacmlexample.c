/*
   Copyright (c) 2005, Andrew McNab and Shiv Kaushal, University of Manchester
   All rights reserved.

   Redistribution and use in source and binary forms, with or
   without modification, are permitted provided that the following
   conditions are met:

     o Redistributions of source code must retain the above
       copyright notice, this list of conditions and the following
       disclaimer. 
     o Redistributions in binary form must reproduce the above
       copyright notice, this list of conditions and the following
       disclaimer in the documentation and/or other materials
       provided with the distribution. 

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

/*---------------------------------------------------------------*
 * For more about GridSite: http://www.gridsite.org/             *
 *---------------------------------------------------------------*/

/*
   Example program using XACML

   Build with:
   
    gcc -o xacmlexample xacmlexample.c -L. -I. -lgridsite -lxml2 -lz -lm
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gridsite.h>

int main()
{
  GRSTgaclCred  *cred, *usercred;
  GRSTgaclEntry *entry;
  GRSTgaclAcl   *acl1, *acl2;
  GRSTgaclUser  *user;
  GRSTgaclPerm   perm0, perm1, perm2;
  FILE          *fp;

  /* must initialise GACL before using XACML functions */

  GRSTgaclInit();

  /* build up an ACL, starting with a credential */

  cred = GRSTgaclCredNew("person");

  GRSTgaclCredAddValue(cred, "dn", "/O=Grid/CN=Mr Grid Person");

  /* create an entry to put it in */

  entry = GRSTgaclEntryNew();

  /* add the credential to it */

  GRSTgaclEntryAddCred(entry, cred);

  /* add another credential */

  cred = GRSTgaclCredNew("dn-list");
  GRSTgaclCredAddValue(cred, "url", "example-dn-list");
  GRSTgaclEntryAddCred(entry, cred);

  fp = fopen("example-dn-list", "w");
  fputs("/O=Grid/CN=Mr Grid Person\n", fp);
  fclose(fp);

  /* associate some permissions and denials to the credential */

  GRSTgaclEntryAllowPerm( entry, GRST_PERM_READ);
  GRSTgaclEntryAllowPerm( entry, GRST_PERM_WRITE);
  GRSTgaclEntryAllowPerm( entry, GRST_PERM_ADMIN);
  GRSTgaclEntryDenyPerm(  entry, GRST_PERM_ADMIN);
  GRSTgaclEntryDenyPerm(  entry, GRST_PERM_LIST);

  perm0 = GRST_PERM_READ | GRST_PERM_WRITE;

  printf("test perm should be %d\n", perm0);

  /* create a new ACL and add the entry to it */

  acl1 = GRSTgaclAclNew();

  GRSTgaclAclAddEntry(acl1, entry);

  /* create a GRSTgaclUser to compare with the ACL */

  usercred = GRSTgaclCredNew("person");

  GRSTgaclCredAddValue(usercred, "dn", "/O=Grid/CN=Mr Grid Person");

  user = GRSTgaclUserNew(usercred);

  GRSTgaclUserSetDNlists(user, getcwd(NULL, 0));
  printf("DN Lists dir %s\n", getcwd(NULL, 0));

//  putenv("GRST_DN_LISTS=.");

  perm1 = GRSTgaclAclTestUser(acl1, user);

  printf("test /O=Grid/CN=Mr Grid Person in acl = %d\n", perm1);

  /* print and save the whole ACL */

  GRSTgaclAclPrint(acl1, stdout);

  GRSTxacmlAclSave(acl1, "example.xacml");

  puts("gridacl.out saved");

  puts("");

  /* load the ACL back off the disk, print and test it */

  acl2 = GRSTxacmlAclLoadFile("example.xacml");

  puts("gridacl.out loaded");

  if (acl2 != NULL) GRSTgaclAclPrint(acl2, stdout); else puts("acl2 is NULL");

  perm2 = GRSTgaclAclTestUser(acl2, user);

  printf("test /O=Grid/CN=Mr Grid Person in acl = %d\n", perm2);

  if (perm1 != perm0) return 1;
  if (perm2 != perm0) return 2;

  return 0;
}

