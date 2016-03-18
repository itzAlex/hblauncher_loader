#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <3ds.h>

extern u32 PAYLOAD_TEXTADDR[];
extern u32 PAYLOAD_TEXTMAXSIZE;

extern Handle gspGpuHandle;

u8 *filebuffer;
u32 filebuffer_maxsize;

char regionids_table[7][4] = {//http://3dbrew.org/wiki/Nandrw/sys/SecureInfo_A
"JPN",
"USA",
"EUR",
"JPN", //"AUS"
"CHN",
"KOR",
"TWN"
};

void gxlowcmd_4(u32* inadr, u32* outadr, u32 size, u32 width0, u32 height0, u32 width1, u32 height1, u32 flags)
{
	GX_TextureCopy(inadr, width0 | (height0<<16), outadr, width1 | (height1<<16), size, flags);
}

Result gsp_flushdcache(u8* adr, u32 size)
{
	return GSPGPU_FlushDataCache(adr, size);
}

Result http_getactual_payloadurl(char *requrl, char *outurl, u32 outurl_maxsize)
{
	Result ret=0;
	httpcContext context;

	ret = httpcOpenContext(&context, HTTPC_METHOD_GET, requrl, 1);
	if(ret!=0)return ret;

	ret = httpcAddRequestHeaderField(&context, "User-Agent", "hblauncher_loader/"VERSION);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcBeginRequest(&context);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcGetResponseHeader(&context, "Location", outurl, outurl_maxsize);

	httpcCloseContext(&context);

	return 0;
}

Result http_download_payload(char *url, u32 *payloadsize)
{
	Result ret=0;
	u32 statuscode=0;
	u32 contentsize=0;
	httpcContext context;

	ret = httpcOpenContext(&context, HTTPC_METHOD_GET, url, 1);
	if(ret!=0)return ret;

	ret = httpcAddRequestHeaderField(&context, "User-Agent", "hblauncher_loader/"VERSION);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcBeginRequest(&context);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcGetResponseStatusCode(&context, &statuscode, 0);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	if(statuscode!=200)
	{
		printf("Error: El servidor ha devuelto el siguiente c�digo %u.\n", (unsigned int)statuscode);
		httpcCloseContext(&context);
		return -2;
	}

	ret=httpcGetDownloadSizeState(&context, NULL, &contentsize);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	if(contentsize==0 || contentsize>PAYLOAD_TEXTMAXSIZE)
	{
		printf("Inv�lido HTTP tama�o de contenido: 0x%08x.\n", (unsigned int)contentsize);
		ret = -3;
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcDownloadData(&context, filebuffer, contentsize, NULL);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	httpcCloseContext(&context);

	*payloadsize = contentsize;

	return 0;
}

Result loadsd_payload(char *filepath, u32 *payloadsize)
{
	struct stat filestats;
	FILE *f;
	size_t readsize=0;

	if(stat(filepath, &filestats)==-1)return errno;

	*payloadsize = filestats.st_size;

	if(filestats.st_size==0 || filestats.st_size>PAYLOAD_TEXTMAXSIZE)
	{
		printf("Tama�o inv�lido del payload de la SD: 0x%08x.\n", (unsigned int)filestats.st_size);
		return -3;
	}

	f = fopen(filepath, "r");
	if(f==NULL)return errno;

	readsize = fread(filebuffer, 1, filestats.st_size, f);
	fclose(f);

	if(readsize!=filestats.st_size)
	{
		printf("fread() Fallo en el payload de la SD.\n");
		return -2;
	}

	return 0;
}

Result savesd_payload(char *filepath, u32 payloadsize)
{
	FILE *f;
	size_t writesize=0;

	unlink(filepath);

	f = fopen(filepath, "w+");
	if(f==NULL)
	{
		printf("Fallo al abrir el payload de la SD para escribirlo.\n");
		return errno;
	}

	writesize = fwrite(filebuffer, 1, payloadsize, f);
	fclose(f);

	if(writesize!=payloadsize)
	{
		printf("fwrite() Fallo en el payload de la SD.\n");
		return -2;
	}

	return 0;
}

Result load_hblauncher()
{
	Result ret = 0;
	u8 region=0;
	u8 new3dsflag = 0;

	OS_VersionBin nver_versionbin;
	OS_VersionBin cver_versionbin;

	u32 payloadsize = 0, payloadsize_aligned = 0;
	u32 payload_src = 0;

	char payload_sysver[32];
	char payloadurl[0x80];
	char payload_sdpath[0x80];

	void (*funcptr)(u32*, u32*) = NULL;
	u32 *paramblk = NULL;

	memset(&nver_versionbin, 0, sizeof(OS_VersionBin));
	memset(&cver_versionbin, 0, sizeof(OS_VersionBin));

	memset(payload_sysver, 0, sizeof(payload_sysver));
	memset(payloadurl, 0, sizeof(payloadurl));
	memset(payload_sdpath, 0, sizeof(payload_sdpath));

	printf("Obteniendo versi�n de sistema/informaci�n de sistema etc...\n");

	ret = cfguInit();
	if(ret!=0)
	{
		printf("Fallo en init cfgu: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}
	ret = CFGU_SecureInfoGetRegion(&region);
	if(ret!=0)
	{
		printf("Fallo en conseguir la region en la cfgu: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}
	if(region>=7)
	{
		printf("El valor de la region en cfgu es inv�lido: 0x%02x.\n", (unsigned int)region);
		ret = -9;
		return ret;
	}
	cfguExit();

	APT_CheckNew3DS(&new3dsflag);

	ret = osGetSystemVersionData(&nver_versionbin, &cver_versionbin);
	if(ret!=0)
	{
		printf("Fallo en conseguir la versi�n de sistema: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	snprintf(payload_sysver, sizeof(payload_sysver)-1, "%s-%d-%d-%d-%d-%s", new3dsflag?"NEW":"OLD", cver_versionbin.mainver, cver_versionbin.minor, cver_versionbin.build, nver_versionbin.mainver, regionids_table[region]);
	snprintf(payloadurl, sizeof(payloadurl)-1, "http://smea.mtheall.com/get_payload.php?version=%s", payload_sysver);
	snprintf(payload_sdpath, sizeof(payload_sdpath)-1, "sdmc:/hblauncherloader_otherapp_payload_%s.bin", payload_sysver);

	printf("Detectada versi�n de sistema: %s %d.%d.%d-%d %s\n", new3dsflag?"New3DS":"Old3DS", cver_versionbin.mainver, cver_versionbin.minor, cver_versionbin.build, nver_versionbin.mainver, regionids_table[region]);

	memset(filebuffer, 0, filebuffer_maxsize);

	hidScanInput();

	if((hidKeysHeld() & KEY_X) == 0)
	{
		printf("El boton X no esta presionado, comprobando el otherapp payload en la SD, con la siguiente ruta de archivo: %s\n", payload_sdpath);
		ret = loadsd_payload(payload_sdpath, &payloadsize);
	}
	else
	{
		printf("Esquivando la carga del payload de la SD porque el b�ton X esta presionado.\n");
		ret = 1;
	}

	if(ret==0)
	{
		printf("El otherapp payload para esta aplicaci�n ya existe, va a ser usado en vez de descargarlo.\n");
		payload_src = 0;
	}
	else
	{
		printf("Solicitando el actual URL con HTTP...\n");
		ret = http_getactual_payloadurl(payloadurl, payloadurl, sizeof(payloadurl));
		if(ret!=0)
		{
			printf("Fallo en solicitar el actual payload URL: 0x%08x.\n", (unsigned int)ret);
			if(ret==0xd8a0a046)
			{
				printf("No hay conexi�n a Internet utilizable.\n");
			}
			else
			{
				printf("Si el server no esta ca�do, y la solicitud HTTP �s actualmente correcta, esto puede significar que su sistema de versi�n o regi�n no est� soportado por el hblauncher-payload actualmente.\n");
			}
			return ret;
		}

		printf("Descargando el actual payload con HTTP...\n");
		ret = http_download_payload(payloadurl, &payloadsize);
		if(ret!=0)
		{
			printf("Fallo en descargar el actual payload con HTTP: 0x%08x.\n", (unsigned int)ret);
			if(ret==0xd8a0a046)
			{
				printf("No hay conexi�n a Internet utilizable.\n");
			}
			else
			{
				printf("Si el server no esta ca�do, y la solicitud HTTP �s actualmente correcta, esto puede significar que su sistema de versi�n o regi�n no est� soportado por el hblauncher-payload actualmente.\n");
			}
			return ret;
		}

		if(ret==0)payload_src = 1;
	}

	printf("Iniciando payload data etc...\n");

	payloadsize_aligned = (payloadsize + 0xfff) & ~0xfff;
	if(payloadsize_aligned > PAYLOAD_TEXTMAXSIZE)
	{
		printf("Tama�o del payload invalido: 0x%08x.\n", (unsigned int)payloadsize);
		ret = -3;
		return ret;
	}

	if(payload_src)
	{
		hidScanInput();

		if(!(hidKeysHeld() & KEY_Y))
		{
			printf("Guardando el payload en la SD porque el boton Y no esta presionado...\n");
			ret = savesd_payload(payload_sdpath, payloadsize);

			if(ret!=0)
			{
				printf("Guardado del payload fallado: 0x%08x.\n", (unsigned int)ret);
			}
			else
			{
				printf("Guardado del payload exitosamente.\n");
			}
		}
		else
		{
			printf("Esquivando guardar el payload en la SD porque el boton Y esta presionado.\n");
		}
	}

	memcpy(PAYLOAD_TEXTADDR, filebuffer, payloadsize_aligned);
	memset(filebuffer, 0, filebuffer_maxsize);

	ret = svcFlushProcessDataCache(0xffff8001, PAYLOAD_TEXTADDR, payloadsize_aligned);//Flush dcache for the payload which was copied into .text. Since that area was never executed, icache shouldn't be an issue.
	if(ret!=0)
	{
		printf("svcFlushProcessDataCache fallado: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	paramblk = linearMemAlign(0x10000, 0x1000);
	if(paramblk==NULL)
	{
		ret = 0xfe;
		printf("Fallo en localizar el pr�ambulo.\n");
		return ret;
	}

	httpcExit();

	memset(paramblk, 0, 0x10000);

	paramblk[0x1c>>2] = (u32)gxlowcmd_4;
	paramblk[0x20>>2] = (u32)gsp_flushdcache;
	paramblk[0x48>>2] = 0x8d;//flags
	paramblk[0x58>>2] = (u32)&gspGpuHandle;

	printf("Saltando en el payload...\n");

	funcptr = (void*)PAYLOAD_TEXTADDR;
	funcptr(paramblk, (u32*)(0x10000000-0x1000));

	ret = 0xff;
	printf("El payload ha vuelto atr�s en la app, esto *nunca* pasa con el actual hblauncher-payload.\n");

	return ret;
}

int main(int argc, char **argv)
{
	Result ret = 0;
	u32 pos;
	Handle kproc_handledup=0;

	// Initialize services
	gfxInitDefault();

	consoleInit(GFX_BOTTOM, NULL);

	printf("hblauncher_loader %s por yellows8. Traducido al espa�ol por @alexpui2002 \n", VERSION);

	ret = svcDuplicateHandle(&kproc_handledup, 0xffff8001);
	if(ret!=0)printf("svcDuplicateHandle() con el siguiente proc-handle fall�: 0x%08x.\n", (unsigned int)ret);

	if(ret==0)
	{
		for(pos=0; pos<PAYLOAD_TEXTMAXSIZE; pos+=0x1000)
		{
			ret = svcControlProcessMemory(kproc_handledup, (u32)&PAYLOAD_TEXTADDR[pos >> 2], 0x0, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
			if(ret!=0)
			{
				printf("svcControlProcessMemory con pos=0x%x fall�: 0x%08x.\n", (unsigned int)pos, (unsigned int)ret);
				break;
			}
		}
	}

	ret = httpcInit(0);
	if(ret!=0)
	{
		printf("Fallo al iniciar HTTPC: 0x%08x.\n", (unsigned int)ret);
		if(ret==0xd8e06406)
		{
			printf("El servicio HTTPC �s inaccesible.\n");
		}
	}

	if(ret==0)
	{
		filebuffer_maxsize = PAYLOAD_TEXTMAXSIZE;

		filebuffer = (u8*)malloc(filebuffer_maxsize);
		if(filebuffer==NULL)
		{
			printf("Fallo en encontrar memoria.\n");
			ret = -1;
		}
		else
		{
			memset(filebuffer, 0, filebuffer_maxsize);
		}
	}

	ret = load_hblauncher();

	free(filebuffer);

	httpcExit();

	if(ret!=0 && ret!=0xd8a0a046)printf("Ha ocurrido un error, porfavor reportelo si persiste(o comentar en un tema ya existente si es necesario), con una imagen de su sistema 3DS: https://github.com/yellows8/hblauncher_loader/issues\n");

	printf("Presiona START para salir.\n");
	// Main loop
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break;
	}

	// Exit services
	gfxExit();
	return 0;
}

