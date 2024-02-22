#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <netinet/in.h>
#include <math.h>
#include <postgresql/libpq-fe.h>
#include "cjson/cJSON.h"

#define PORT 8080
#define BUFFER_SIZE 2048
#define MAX_DESC_LENGTH 10

PGconn *conn = NULL;

const char *select_client = "SELECT c.saldo, c.limite, (timezone('America/Sao_Paulo', now())) FROM cliente c WHERE c.id = $1";
const char *update_client = "UPDATE cliente SET saldo = (saldo + $1) WHERE id = $2 AND (saldo + $1) > (-limite) RETURNING saldo, limite";
const char *insert_trancacao = "INSERT INTO transacao (cliente_id, valor, descricao, tipo, realizada_em) VALUES ($1, $2, $3, $4, (timezone('America/Sao_Paulo', now())))";
const char *select_transacao = "SELECT t.valor, t.tipo, t.descricao, t.realizada_em FROM transacao t WHERE t.cliente_id = $1 ORDER BY t.realizada_em DESC LIMIT 10";

void connect_to_db() {
    const char *conninfo = "dbname=rinha user=postgres password=postgres host=db port=5432";
    conn = PQconnectdb(conninfo);

    if (PQstatus(conn) != CONNECTION_OK) {
        printf("Error while connecting to the database server: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        exit(1);
    }
}

void return_response_fail(int client_socket, char *message, int status, char *statusText) {
    char http_response[BUFFER_SIZE];
    snprintf(http_response, BUFFER_SIZE, "HTTP/1.1 %d %s\nContent-Type: text/plain\n\n%s", status, statusText, message);
    write(client_socket, http_response, strlen(http_response));
    close(client_socket);
}

void return_response_ok(int client_socket, char *json_response) {
    char http_response[BUFFER_SIZE];
    snprintf(http_response, BUFFER_SIZE, "HTTP/1.1 200 OK\nContent-Type: application/json\n\n%s", json_response);
    write(client_socket, http_response, strlen(http_response));
    close(client_socket);
}

void disconnect_from_db() {
    PQfinish(conn);
    printf("Disconnected from database.\n");
}

void error_query(PGresult *res, int client_socket) {
    fprintf(stderr, "Query execution failed: %s", PQresultErrorMessage(res));
    PQclear(res);
    conn = NULL;
    close(client_socket);
}

PGresult* execute_params(const int client_socket, const char* query, const char *params[], int size) {
    PGresult *res = PQexecParams(conn, query, size, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        error_query(res, client_socket);
        return NULL;
    }
    return res; 
}

void handle_post_transacoes(int client_socket, const char id[20], int valor, const char* tipo, const char* descricao) {
    if (valor <= 0) {
        return_response_fail(client_socket, "Invalid valor (must be a positive integer)", 422, "Unprocessable Entity");
        return;
    }

    if (descricao[0] == '\0' || strlen(descricao) > MAX_DESC_LENGTH) {
        return_response_fail(client_socket, "Invalid descricao (max length exceeded)", 422, "Unprocessable Entity");
        return;
    }

    if (tolower(tipo[0]) != 'c' && tolower(tipo[0]) != 'd') {
        return_response_fail(client_socket, "Invalid tipo (must be 'c' or 'd')", 422, "Unprocessable Entity");
        return;
    }

    
    int novoValor = tolower(tipo[0]) == 'c' ? valor : valor * -1;
    char buffer[100]; 
    snprintf(buffer, sizeof(buffer), "%d", novoValor);
    
    PGresult *res = PQexecParams(conn, update_client, 2, NULL, (const char *[]){buffer, id}, NULL, NULL, 0);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        // Verificar se a consulta UPDATE foi bem-sucedida e retornou uma única linha
        return_response_fail(client_socket, "Transação rejeitada", 422, "Unprocessable Entity");
        PQclear(res);
        return;
    }
    int novo_saldo = atoi(PQgetvalue(res, 0, 0));
    int novo_limite = atoi(PQgetvalue(res, 0, 1));
    PQclear(res);

    char bufferValor[100]; 
    snprintf(bufferValor, sizeof(bufferValor), "%d", valor);
    res = PQexecParams(conn, insert_trancacao, 4, NULL, (const char *[]){id, bufferValor, descricao, tipo}, NULL, NULL, 0);
    PQclear(res);
    
    char json_response[BUFFER_SIZE];
    snprintf(json_response, BUFFER_SIZE, "{\n  \"limite\": %d, \"saldo\": %d}", novo_limite, novo_saldo);
    return_response_ok(client_socket, json_response);
}

void handle_get_extrato(int client_socket, char* id) {
    PGresult *res = execute_params(client_socket, select_client, (const char *[]){id}, 1);
    char json_response[BUFFER_SIZE];
    snprintf(json_response, BUFFER_SIZE, "{\n  \"saldo\": {\n    \"total\": %s,\n   \"data_extrato\": \"%s\",\n   \"limite\": %s\n  },\n    \"ultimas_transacoes\": [",
             PQgetvalue(res, 0, 0), PQgetvalue(res, 0, 2), PQgetvalue(res, 0, 1));

    PQclear(res);
    res = execute_params(client_socket, select_transacao, (const char *[]){id}, 1);
    int num_rows = PQntuples(res);
    for (int i = 0; i < num_rows; i++) {
        if (PQgetvalue(res, i, 1) != NULL && strlen(PQgetvalue(res, i, 1)) > 0) {
            snprintf(json_response + strlen(json_response), BUFFER_SIZE - strlen(json_response),
                    "\n    {\n      \"valor\": %s,\n      \"tipo\": \"%s\",\n      \"descricao\": \"%s\",\n      \"realizada_em\": \"%s\"\n    }%s",
                    PQgetvalue(res, i, 0), PQgetvalue(res, i, 1), PQgetvalue(res, i, 2), PQgetvalue(res, i, 3),
                    i < num_rows - 1 ? "," : "");
        }
    }

    snprintf(json_response + strlen(json_response), BUFFER_SIZE - strlen(json_response),
             "\n  ]\n}");

    PQclear(res);
    return_response_ok(client_socket, json_response);
}

int hasDecimal(double num) {
    double intPart;
    return modf(num, &intPart) != 0.0;
}

void handle_request(int client_socket) {
    char buffer[BUFFER_SIZE] = {0};
    if (read(client_socket, buffer, BUFFER_SIZE) <= 0) {
        perror("Error reading request");
        close(client_socket);
        return;
    }
    
    char method[10], endpoint[50], id[20], *body = NULL;
    if (sscanf(buffer, "%9s %49s %*s", method, endpoint) != 2) {
        perror("Error parsing request");
        close(client_socket);
        return;
    }

    sscanf(endpoint, "/clientes/%19[^/]", id);
    int client_id = atoi(id);
    if (client_id < 1 || client_id > 5) {
        return_response_fail(client_socket, "Client not found", 404, "Not Found");
        return;
    }

    if (strcmp(method, "GET") == 0 && strstr(endpoint, "/extrato") != NULL) {
        handle_get_extrato(client_socket, id);
    } else if (strcmp(method, "POST") == 0 && strstr(endpoint, "/transacoes") != NULL) {
        char* body_start = strstr(buffer, "\r\n\r\n");
        if (body_start != NULL) {
            body_start += 4;
            cJSON *json = cJSON_Parse(body_start);
            if (json != NULL) {
                cJSON* valor = cJSON_GetObjectItemCaseSensitive(json, "valor");
                cJSON* tipo = cJSON_GetObjectItemCaseSensitive(json, "tipo");
                cJSON* descricao = cJSON_GetObjectItemCaseSensitive(json, "descricao");

                if(valor != NULL && hasDecimal(valor->valuedouble) == 0 && tipo != NULL && descricao != NULL && cJSON_IsNumber(valor) && cJSON_IsString(tipo) && cJSON_IsString(descricao)) {
                    handle_post_transacoes(client_socket, id, valor->valueint, tipo->valuestring, descricao->valuestring);
                } else {
                    return_response_fail(client_socket, "Invalid JSON body", 422, "Unprocessable Entity");
                }
                cJSON_Delete(json);
            } else {
                return_response_fail(client_socket, "Invalid JSON body", 422, "Unprocessable Entity");
            }
        } else {
            return_response_fail(client_socket, "Body not found", 422, "Unprocessable Entity");
        }
    } else {
        return_response_fail(client_socket, "Not Found", 404, "Not Found");
    }

    close(client_socket);
}

int main() {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    connect_to_db();
    PQexecParams(conn, select_client, 1, NULL, (const char *[]){"1"}, NULL, NULL, 0);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PORT);

    while (1) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed");
            exit(EXIT_FAILURE);
        }
        
        if (conn == NULL) {
            connect_to_db();
        }

        handle_request(client_socket);
    }

    disconnect_from_db();

    return 0;
}
