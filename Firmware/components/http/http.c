//Modified by Shoaib & Shan
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "errno.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "url_parser.h"
#include "http.h"
//https 
#include <stdlib.h>
#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"
#include "audio_player.h"//killing the read
//https*




#define TAG "http_client"


/**
 * @brief simple http_get
 * see https://github.com/nodejs/http-parser for callback usage
 */


/* Root cert for shoaibomar.com, taken from server_root_cert.pem
   The PEM file can be extracted using this command:
   openssl s_client -showcerts -connect url:443 </dev/null
   The CA root cert is the last cert given in the chain of certs.
   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const uint8_t server_root_cert_pem_start[] asm("_binary_server_root_cert_pem_start");
extern const uint8_t server_root_cert_pem_end[]   asm("_binary_server_root_cert_pem_end");

int https_init = 1;
int ret, flags;
char portstr[10];
char buf[512];
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;
mbedtls_ssl_context ssl;
mbedtls_x509_crt cacert;
mbedtls_ssl_config conf;
mbedtls_net_context server_fd;
http_parser parser;


void init_https(url_t *url){
    https_init = 0;
    mbedtls_ssl_init(&ssl);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    ESP_LOGI(TAG, "Seeding the random number generator");

    mbedtls_ssl_config_init(&conf);

    mbedtls_entropy_init(&entropy);
    if((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,NULL, 0)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed returned %d", ret);
        abort();
    }

    ESP_LOGI(TAG, "Loading the CA root certificate...");

    ret = mbedtls_x509_crt_parse(&cacert, server_root_cert_pem_start,server_root_cert_pem_end-server_root_cert_pem_start);

    if(ret < 0)
    {
        ESP_LOGE(TAG, "mbedtls_x509_crt_parse returned -0x%x\n\n", -ret);
        abort();
    }

    ESP_LOGI(TAG, "Setting hostname for TLS session...");
    /* Hostname set here should match CN in server certificate */
    if((ret = mbedtls_ssl_set_hostname(&ssl, url->host)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_set_hostname returned -0x%x", -ret);
        abort();
    }
    ESP_LOGI(TAG, "Setting up the SSL/TLS structure...");
    if((ret = mbedtls_ssl_config_defaults(&conf,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_config_defaults returned %d", ret);
        goto exit;
    }
    /* MBEDTLS_SSL_VERIFY_OPTIONAL is bad for security, in this example it will print a warning if CA verification fails but it will continue to connect. You should consider using MBEDTLS_SSL_VERIFY_REQUIRED in your own code.*/
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    #ifdef CONFIG_MBEDTLS_DEBUG
        mbedtls_esp_enable_debug_log(&conf, 4);
    #endif
    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_setup returned -0x%x\n\n", -ret);
        goto exit;
    }
    exit:
    mbedtls_ssl_session_reset(&ssl);
    mbedtls_net_free(&server_fd);
}


int http_client_get(char *uri, http_parser_settings *callbacks, void *user_data)
{
    int is_https = 0;
    url_t *url = url_parse(uri);
    if (strstr(url->scheme,"https") != NULL){
        //is https
        is_https = 1;
        ESP_LOGI(TAG, "------\nHTTPS=%s://%s:%d\n------", url->scheme,url->host,url->port);
        if (https_init){
            init_https(url);
        }
        //is https
        sprintf(portstr, "%u", url->port);
        //Perform connection
        mbedtls_net_init(&server_fd);
        ESP_LOGI(TAG, "Connecting to %s:%s...", url->host,portstr);
        if ((ret = mbedtls_net_connect(&server_fd, url->host,portstr, MBEDTLS_NET_PROTO_TCP)) != 0)
        {
            ESP_LOGE(TAG, "mbedtls_net_connect returned -%x", -ret);
            goto exit;
        }
        ESP_LOGI(TAG, "Connected.");
        mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);
        ESP_LOGI(TAG, "Performing the SSL/TLS handshake...");
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
        {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                ESP_LOGE(TAG, "mbedtls_ssl_handshake returned -0x%x", -ret);
                goto exit;
            }
        }
        ESP_LOGI(TAG, "Verifying peer X.509 certificate...");
        if ((flags = mbedtls_ssl_get_verify_result(&ssl)) != 0)
        {
            /* In real life, we probably want to close connection if ret != 0 */
            ESP_LOGW(TAG, "Failed to verify peer certificate!");
            bzero(buf, sizeof(buf));
            mbedtls_x509_crt_verify_info(buf, sizeof(buf), "  ! ", flags);
            ESP_LOGW(TAG, "verification info: %s", buf);
        }
        else {
            ESP_LOGI(TAG, "Certificate verified.");
        }

        ESP_LOGI(TAG, "Cipher suite is %s", mbedtls_ssl_get_ciphersuite(&ssl));

        ESP_LOGI(TAG, "Writing HTTP request...");
        char *request;
        if(asprintf(&request, "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: ESP32\r\nAccept: */*\r\n\r\n", url->path, url->host, url->port) < 0)
        {
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "requesting %s", request);
        size_t written_bytes = 0;
        do {
            ret = mbedtls_ssl_write(&ssl,(const unsigned char *)request + written_bytes,strlen(request) - written_bytes);
            if (ret >= 0) {
                ESP_LOGI(TAG, "%d bytes written", ret);
                written_bytes += ret;
            } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_WANT_READ) {
                ESP_LOGE(TAG, "mbedtls_ssl_write returned -0x%x", -ret);
                goto exit;
            }
        } while(written_bytes < strlen(request));
        ESP_LOGI(TAG, "Reading HTTP response...");
        /* Read HTTP response */
        char recv_buf[64];
        bzero(recv_buf, sizeof(recv_buf));
        ssize_t recved;

        /* intercept on_headers_complete() */

        /* parse response */
        http_parser_init(&parser, HTTP_RESPONSE);
        parser.data = user_data;

        esp_err_t nparsed = 0;
        reset_player_status();//hack to make it work
        do {
            bzero(recv_buf, sizeof(recv_buf));
            recved = mbedtls_ssl_read(&ssl, (unsigned char *)recv_buf, sizeof(recv_buf)-1);

            // using http parser causes stack overflow somtimes - disable for now
            nparsed = http_parser_execute(&parser, callbacks, recv_buf, recved);

            // invoke on_body cb directly
            // nparsed = callbacks->on_body(&parser, recv_buf, recved);
        } while(recved > 0 && nparsed >= 0 && (get_player_status() != STOPPED));
        mbedtls_ssl_close_notify(&ssl);
        //}

    }else{
        //HTTP
        const struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
        };
        struct addrinfo *res;
        struct in_addr *addr;
        char port_str[6]; // stack allocated
        snprintf(port_str, 6, "%d", url->port);

        int err = getaddrinfo(url->host, port_str, &hints, &res);
        if(err != ESP_OK || res == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
            return err;
        }

        // print resolved IP
        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        // allocate socket
        int sock = socket(res->ai_family, res->ai_socktype, 0);
        if(sock < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
        }
        ESP_LOGI(TAG, "... allocated socket");


        // connect, retrying a few times
        char retries = 0;
        while(connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
            retries++;
            ESP_LOGE(TAG, "... socket connect attempt %d failed, errno=%d", retries, errno);

            if(retries > 5) {
                ESP_LOGE(TAG, "giving up");
                close(sock);
                freeaddrinfo(res);
                return ESP_FAIL;
            }
        }

        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);

        // write http request
        char *request;
        if(asprintf(&request, "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: ESP32\r\nAccept: */*\r\n\r\n", url->path, url->host, url->port) < 0)
        {
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "requesting %s", request);

        if (write(sock, request, strlen(request)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(sock);
        }

        free(request);
        ESP_LOGI(TAG, "... socket send success");


        /* Read HTTP response */
        char recv_buf[64];
        bzero(recv_buf, sizeof(recv_buf));
        ssize_t recved;

        /* intercept on_headers_complete() */

        /* parse response */
        http_parser_init(&parser, HTTP_RESPONSE);
        parser.data = user_data;

        esp_err_t nparsed = 0;
        reset_player_status();//hack to make it work
        do {
            recved = read(sock, recv_buf, sizeof(recv_buf)-1);

            // using http parser causes stack overflow somtimes - disable for now
            nparsed = http_parser_execute(&parser, callbacks, recv_buf, recved);

            // invoke on_body cb directly
            // nparsed = callbacks->on_body(&parser, recv_buf, recved);
        } while(recved > 0 && nparsed >= 0 && (get_player_status() != STOPPED));


        ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d", recved, errno);
        close(sock);
        ESP_LOGI(TAG, "socket closed");
        //HTTP
    }
    exit:
    if (is_https == 1){
        mbedtls_ssl_session_reset(&ssl);
        mbedtls_net_free(&server_fd);
    }
    free(url);
    return 0;
}