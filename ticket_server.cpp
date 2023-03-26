#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <ctime>
#include <string>
#include <random>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>

// Typy.
using port_t = uint16_t;
using timeout_t = uint32_t;
using message_id_t = uint8_t;
using description_length_t = uint8_t;
using ticket_count_t = uint16_t;
using event_id_t = uint32_t;
using reservation_id_t = uint32_t;
using expiration_time_t = uint64_t;
using event_or_reservation_id_t = uint32_t;

// Do parsowania parametrów w linii poleceń.
constexpr port_t DEFAULT_PORT = 2022;
constexpr timeout_t DEFAULT_TIMEOUT = 5;
constexpr int MIN_NUMBER_OF_PARAMETERS = 3;
constexpr const char *FILE_FLAG = "-f";
constexpr const char *PORT_FLAG = "-p";
constexpr const char *TIMEOUT_FLAG = "-t";
constexpr port_t PORT_MAX = 65535;
constexpr timeout_t TIMEOUT_MIN = 1;
constexpr timeout_t TIMEOUT_MAX = 86400;
constexpr char NULL_TERMINATOR = '\0';
constexpr int DECIMAL_BASE = 10;

// Rozmiar bufora (jeden datagram UDP).
constexpr size_t BUFFER_SIZE = 65507;

// Nazwy komunikatów.
constexpr message_id_t GET_EVENTS = 1;
constexpr message_id_t EVENTS = 2;
constexpr message_id_t GET_RESERVATION = 3;
constexpr message_id_t RESERVATION = 4;
constexpr message_id_t GET_TICKETS = 5;
constexpr message_id_t TICKETS = 6;
constexpr message_id_t BAD_REQUEST = 255;
constexpr const char *EVENTS_MESSAGE_NAME = "EVENTS";
constexpr const char *RESERVATION_MESSAGE_NAME = "RESERVATION";
constexpr const char *TICKETS_MESSAGE_NAME = "TICKETS";
constexpr const char *BAD_REQUEST_MESSAGE_NAME = "BAD_REQUEST";

// Rozmiary pól występujących w komunikatach między klientem a serwerem.
constexpr size_t MESSAGE_ID_SIZE = 1;
constexpr size_t DESCRIPTION_LENGTH_SIZE = 1;
constexpr size_t TICKET_COUNT_SIZE = 2;
constexpr size_t EVENT_ID_SIZE = 4;
constexpr size_t RESERVATION_ID_SIZE = 4;
constexpr size_t COOKIE_SIZE = 48;
constexpr size_t EXPIRATION_TIME_SIZE = 8;
constexpr size_t TICKET_SIZE = 7;
constexpr size_t EVENT_OR_RESERVATION_ID_SIZE = 4;

// Rozmiary poprawnych komunikatów od klienta.
constexpr size_t GET_EVENTS_MESSAGE_LENGTH = MESSAGE_ID_SIZE;
constexpr size_t GET_RESERVATION_MESSAGE_LENGTH = MESSAGE_ID_SIZE + EVENT_ID_SIZE + TICKET_COUNT_SIZE;
constexpr size_t GET_TICKETS_MESSAGE_LENGTH = MESSAGE_ID_SIZE + RESERVATION_ID_SIZE + COOKIE_SIZE;

// Największa liczba biletów, jaką serwer jest w stanie wysłać klientowi.
constexpr ticket_count_t MAX_TICKETS_TO_SEND = (BUFFER_SIZE - MESSAGE_ID_SIZE - RESERVATION_ID_SIZE
                                                                 - TICKET_COUNT_SIZE) / TICKET_SIZE;

// Do obsługi rezerwacji i biletów.
constexpr reservation_id_t FIRST_RESERVATION_ID = 1000000;
constexpr int FIRST_TICKET_ID = 0;
constexpr int COOKIE_MIN_ASCII_CODE = 33;
constexpr int COOKIE_MAX_ASCII_CODE = 126;

// Rozmiary typów wymagających konwersji na porządek hosta/sieciowy.
constexpr size_t TWO_OCTETS = sizeof(uint16_t);
constexpr size_t FOUR_OCTETS = sizeof(uint32_t);
constexpr size_t EIGHT_OCTETS = sizeof(uint64_t);

// Bufor.
char shared_buffer[BUFFER_SIZE];

// Do debugowania z flagą DNDEBUG.
#ifndef NDEBUG
const bool debug = true;
#else
const bool debug = false;
#endif

// Źródło: materiały z laboratorium.
// Jeśli 'x' nie zachodzi, wypisuje komunikat o błędzie i kończy działanie programu.
#define ENSURE(x)                                                         \
    do {                                                                  \
        bool result = (x);                                                \
        if (!result) {                                                    \
            fprintf(stderr, "Error: %s was false in %s at %s:%d\n",       \
                #x, __func__, __FILE__, __LINE__);                        \
            exit(EXIT_FAILURE);                                           \
        }                                                                 \
    } while (0)

// Źródło: materiały z laboratorium.
// Sprawdza, czy errno jest niezerowe i jeśli tak, wypisuje komunikat o błędzie i kończy
// działanie programu.
#define PRINT_ERRNO()                                                  \
    do {                                                               \
        if (errno != 0) {                                              \
            fprintf(stderr, "Error: errno %d in %s at %s:%d\n%s\n",    \
              errno, __func__, __FILE__, __LINE__, strerror(errno));   \
            exit(EXIT_FAILURE);                                        \
        }                                                              \
    } while (0)

// Źródło: materiały z laboratorium.
// Zeruje 'errno' i wykonuje 'x'. Jeśli 'errno' się zmieniło, opisuje je i kończy
// działanie programu.
#define CHECK_ERRNO(x)                                                             \
    do {                                                                           \
        errno = 0;                                                                 \
        (void) (x);                                                                \
        PRINT_ERRNO();                                                             \
    } while (0)

// Źródło: materiały z laboratorium.
// Wypisuje komunikat błędu i kończy działanie programu.
inline static void fatal(const char *fmt, ...) {
    va_list fmt_args;
    fprintf(stderr, "Error: ");
    va_start(fmt_args, fmt);
    vfprintf(stderr, fmt, fmt_args);
    va_end(fmt_args);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

// Sprawdza, czy liczba jest parzysta.
bool is_even(int number) {
    return number % 2 == 0;
}

// Sprawdza, czy parametr (parameter) jest flagą (flag).
bool is_a_flag(const char *parameter, const char *flag) {
    return strcmp(parameter, flag) == 0;
}

// Sprawdza, czy string jest poprawną ścieżką do pliku, jeśli nie, wypisuje komunikat o błędzie
// i kończy działanie programu, w przeciwnym przypadku zwraca go.
char *read_path(char *string) {
    struct stat buffer{};
    if (stat(string, &buffer) != 0) {
        fatal("%s is not a valid path to file", string);
    }
    if (!S_ISREG(buffer.st_mode)) {
        fatal("%s is not a valid path to file", string);
    }
    return string;
}

// Sprawdza, czy string jest poprawnym portem, jeśli nie, wypisuje komunikat o błędzie
// i kończy działanie programu, w przeciwnym przypadku zwraca go w odpowiednim typie.
port_t read_port(char *string) {
    errno = 0;
    char *end_ptr;
    unsigned long port = strtoul(string, &end_ptr, DECIMAL_BASE);
    PRINT_ERRNO();
    if (port > PORT_MAX || *end_ptr != NULL_TERMINATOR) {
        fatal("%s is not a valid port number", string);
    }
    return (port_t) port;
}

// Sprawdza, czy string jest poprawnym limitem czasu, jeśli nie, wypisuje komunikat o błędzie
// i kończy działanie programu, w przeciwnym przypadku zwraca go w odpowiednim typie.
timeout_t read_timeout(char *string) {
    errno = 0;
    char *end_ptr;
    unsigned long timeout = strtoul(string, &end_ptr, DECIMAL_BASE);
    PRINT_ERRNO();
    if (timeout < TIMEOUT_MIN || timeout > TIMEOUT_MAX || *end_ptr != NULL_TERMINATOR) {
        fatal("%s is not a valid timeout value", string);
    }
    return (timeout_t) timeout;
}

// Funkcja parsująca parametry podane w linii poleceń. Zapisuje odczytane wartości w odpowiednich
// zmiennych. Jeśli liczba parametrów, rodzaje flag lub dowolny parametr nie są poprawne, albo
// jeśli ścieżka do pliku nie została podana, wypisuje komunikat o błędzie i kończy działanie programu.
void parse_parameters(int number_of_parameters, char *parameters[], char *(&path), port_t &port,
                                                                             timeout_t &timeout) {
    if (number_of_parameters < MIN_NUMBER_OF_PARAMETERS ||
        is_even(number_of_parameters)) {
        fatal("%d is not a valid number of parameters\n"
              "Usage: %s -f <path to events file> [-p <port>] [-t <timeout>]",
              number_of_parameters - 1, parameters[0]);
    }

    port = DEFAULT_PORT;
    timeout = DEFAULT_TIMEOUT;

    bool found_file_flag = false;
    for (int i = 1; i < number_of_parameters; i += 2) {
        if (is_a_flag(parameters[i], FILE_FLAG)) {
            found_file_flag = true;
            path = read_path(parameters[i + 1]);
        } else if (is_a_flag(parameters[i], PORT_FLAG)) {
            port = read_port(parameters[i + 1]);
        } else if (is_a_flag(parameters[i], TIMEOUT_FLAG)) {
            timeout = read_timeout(parameters[i + 1]);
        } else {
            fatal("%s is not a valid flag", parameters[i]);
        }
    }

    if (!found_file_flag) {
        fatal("Path to file parameter must be defined\n"
              "Usage: %s -f <path to events file> [-p <port>] [-t <timeout>]", parameters[0]);
    }
}

// Źródło: materiały z laboratorium.
// Funkcja wiążąca dany port z gniazdem.
int bind_socket(port_t port) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
    ENSURE(socket_fd > 0);
    // after socket() call; we should close(sock) on any execution path;

    struct sockaddr_in server_address{};
    server_address.sin_family = AF_INET; // IPv4
    server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
    server_address.sin_port = htons(port);

    // bind the socket to a concrete address
    CHECK_ERRNO(bind(socket_fd, (struct sockaddr *) &server_address,
                     (socklen_t) sizeof(server_address)));

    return socket_fd;
}

// Źródło: materiały z laboratorium.
// Funkcja odbierająca od klienta wiadomość z bufora.
size_t read_message(int socket_fd, struct sockaddr_in *client_address, char *buffer, size_t max_length) {
    auto address_length = (socklen_t) sizeof(*client_address);
    int flags = 0; // we do not request anything special
    errno = 0;
    ssize_t len = recvfrom(socket_fd, buffer, max_length, flags,
                           (struct sockaddr *) client_address, &address_length);
    if (len < 0) {
        PRINT_ERRNO();
    }
    auto read_length = (size_t) len;
    char *client_ip = inet_ntoa(client_address->sin_addr);
    port_t client_port = ntohs(client_address->sin_port);
    if (debug) {
        fprintf(stderr, "Received %zd bytes from client [%s:%u]\n",
                read_length, client_ip, client_port);
    }
    return (size_t) len;
}

// Źródło: materiały z laboratorium.
// Funkcja wysyłająca do klienta wiadomość poprzez bufor.
void send_message(int socket_fd, const struct sockaddr_in *client_address, const char *message,
                                                size_t length, const std::string &message_name) {
    auto address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;
    ssize_t sent_length = sendto(socket_fd, message, length, flags,
                                 (struct sockaddr *) client_address, address_length);
    ENSURE(sent_length == (ssize_t) length);
    char *client_ip = inet_ntoa(client_address->sin_addr);
    port_t client_port = ntohs(client_address->sin_port);
    if (debug) {
        fprintf(stderr, "Sent %s message to client [%s:%u]\n",
                message_name.c_str(), client_ip, client_port);
    }
}

// Funkcja odczytująca z bufora ciąg size bajtów i zapisująca je w zmiennej value. W razie potrzeby
// konwertuje odczytaną wartość na porządek hosta. Odpowiednio zwiększa wartość parsed_length.
template<typename T>
void get_from_buffer(T &value, size_t size, char *buffer, size_t &parsed_length) {
    T value_network_byte_order;
    memcpy(&value_network_byte_order, buffer + parsed_length, size);
    parsed_length += size;
    switch (size) {
        case TWO_OCTETS:
            value = ntohs(value_network_byte_order);
            break;
        case FOUR_OCTETS:
            value = ntohl(value_network_byte_order);
            break;
        default:
            value = value_network_byte_order;
            break;
    }
}

// Funkcja odczytująca z bufora ciąg size bajtów i zapisująca je w zmiennej value typu string.
// Odpowiednio zwiększa wartość parsed_length.
void get_from_buffer(std::string &value, size_t size, char *buffer, size_t &parsed_length) {
    value.resize(size);
    memcpy(&value[0], buffer + parsed_length, size);
    parsed_length += size;
}

// Funkcja wstawiająca do bufora ciąg size bajtów o wartości zmiennej value. W razie potrzeby
// konwertuje wstawianą wartość na porządek sieciowy. Odpowiednio zwiększa wartość send_length.
template<typename T>
void insert_to_buffer(T value, size_t size, char *buffer, size_t &send_length) {
    T value_network_byte_order;
    switch (size) {
        case TWO_OCTETS:
            value_network_byte_order = htons(value);
            break;
        case FOUR_OCTETS:
            value_network_byte_order = htonl(value);
            break;
        case EIGHT_OCTETS:
            value_network_byte_order = htobe64(value);
            break;
        default:
            value_network_byte_order = value;
            break;
    }
    memcpy(buffer + send_length, &value_network_byte_order, size);
    send_length += size;
}

// Funkcja wstawiająca do bufora ciąg size bajtów o wartości zmiennej value typu string.
// Odpowiednio zwiększa wartość send_length.
void insert_to_buffer(const std::string &value, size_t size, char *buffer, size_t &send_length) {
    memcpy(buffer + send_length, &value[0], size);
    send_length += size;
}

// Struktura opisująca wydarzenie.
struct Event {
    std::string description;
    ticket_count_t ticket_count;
    description_length_t description_length;
    size_t size; // Ile bajtów zajmuje to wydarzenie przy wstawianiu do bufora zgodnie ze
                // schematem komunikatu EVENTS (rozmiar event_id + rozmiar ticket_count
                // + rozmiar description_length + rozmiar description).
};

// Oblicza rozmiar wydarzenia (pole size - jak wyżej).
size_t calculate_event_size(description_length_t description_length) {
    return EVENT_ID_SIZE + TICKET_COUNT_SIZE + DESCRIPTION_LENGTH_SIZE + description_length;
}

// Wczytuje wydarzenia z pliku podanego w ścieżce path i zapisuje je w wektorze event w formie
// struktur Event. Jeśli plik nie istnieje lub wystąpi błąd przy otwieraniu pliku, wypisuje
// komunikat o błędzie i kończy działanie programu.
void load_events(char *path, std::vector<Event> &events) {
    std::ifstream file(path);
    if (!file.is_open() || file.fail()) {
        fatal("There is no such file as %s", path);
    }
    std::string description;
    ticket_count_t ticket_count;
    while (getline(file,description)) {
        file >> ticket_count;
        description_length_t description_length = description.length();
        size_t size = calculate_event_size(description_length);
        events.push_back({description, ticket_count, description_length, size});
        file.ignore();
    }
    file.close();
}

// Struktura generująca unikalne bilety. Traktuje każdy bilet jako kolejną liczbę zapisaną
// w systemie o podstawie równej długości alfabetu, nad którym generowane są bilety. Numer
// w kolejności generowanego biletu jest wyznaczany przez identyfikator next_ticket_id.
class TicketGenerator {
    size_t next_ticket_id;
    constexpr static const char ticket_alphabet[] =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t ticket_alphabet_size = strlen(ticket_alphabet);

public:
    TicketGenerator(): next_ticket_id(FIRST_TICKET_ID) {}

    // Zwraca następny ticket_id.
    size_t generate_ticket_id() {
        return next_ticket_id++;
    }

    // Generuje kolejny bilet.
    std::string generate_ticket() {
        std::string ticket;
        ticket.resize(TICKET_SIZE);
        size_t ticket_id = generate_ticket_id();
        for (int i = TICKET_SIZE - 1; i >= 0; i--) {
            ticket[i] = ticket_alphabet[ticket_id % ticket_alphabet_size];
            ticket_id /= ticket_alphabet_size;
        }
        return ticket;
    }
};

// Struktura opisująca rezerwację.
struct Reservation {
    event_id_t event_id;
    ticket_count_t ticket_count;
    std::string cookie;
    expiration_time_t expiration_time;
    bool claimed_tickets; // Flaga mówiąca o tym, czy serwer już co najmniej raz wysłał bilety.
    std::vector<std::string> tickets;
};

// Struktura zajmująca się rezerwacjami.
class ReservationsHandler {
    std::unordered_map<reservation_id_t, Reservation> reservations_map;
    reservation_id_t next_reservation_id;
    std::vector<Event> *events;
    std::queue<reservation_id_t> reservations_queue; // Kolejka reservation_id służąca do usuwania
                                                    // przeterminowanych rezerwacji.
    timeout_t timeout;
    TicketGenerator ticket_generator;
    // Niezmiennik mapy: po wywołaniu funkcji remove_expired_reservations w mapie znajdują się
    // rezerwacje, które zostały już zrealizowane (serwer co najmniej raz wysłał bilety) oraz te,
    // których expiration_time jeszcze nie minął.
    // Niezmiennik kolejki: po wywołaniu funkcji remove_expired_reservations w kolejce znajdują się
    // identyfikatory rezerwacji, których expiration_time jeszcze nie minął.

public:
    explicit ReservationsHandler(std::vector<Event> *events, timeout_t timeout):
            next_reservation_id(FIRST_RESERVATION_ID), events(events), timeout(timeout),
            ticket_generator() {}

    // Zwraca następny reservation_id.
    reservation_id_t generate_reservation_id() {
        return next_reservation_id++;
    }

    // Generuje cookie, losując każdy znak z odpowiedniego zakresu.
    static std::string generate_cookie() {
        std::string cookie;
        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<> dist(COOKIE_MIN_ASCII_CODE, COOKIE_MAX_ASCII_CODE);
        for (size_t i = 0; i < COOKIE_SIZE; i++) {
            cookie += (char) dist(generator);
        }
        return cookie;
    }

    // Dodaje do map rezerwacji nową rezerwację o danych parametrach, rezerwuje odpowiednią
    // liczbę biletów. Dodaje reservation_id tej rezerwacji do kolejki i zwraca je.
    reservation_id_t add_reservation(event_id_t event_id, ticket_count_t ticket_count,
                                                       expiration_time_t current_time) {
        reservation_id_t reservation_id = generate_reservation_id();
        std::vector<std::string> tickets;
        reservations_map.insert({reservation_id,
                                 {event_id, ticket_count, generate_cookie(),
                                  current_time + timeout, false, tickets}});
        (*events)[event_id].ticket_count -= ticket_count;
        reservations_queue.push(reservation_id);
        return reservation_id;
    }

    // Zdejmuje z kolejki rezerwacje, których czas na odebranie minął. Jeśli klient nie odebrał
    // biletów z danej rezerwacji, zwraca odpowiednią liczbę biletów do puli i usuwa rezerwację
    // z mapy.
    void remove_expired_reservations(expiration_time_t current_time) {
        while (!reservations_queue.empty()) {
            reservation_id_t reservation_id = reservations_queue.front();
            Reservation &reservation = reservations_map[reservation_id];
            if (reservation.expiration_time >= current_time) {
                // W kolejce rezerwacje są ustawione od najwcześniejszego expiration_time do
                // najpóźniejszego, więc jeśli dana rezerwacja jeszcze się nie przeterminowała,
                // wszystkie za nią w kolejce również nie są przeterminowane, możemy więc
                // wyjść z funkcji.
                return;
            }
            reservations_queue.pop();
            if (!reservation.claimed_tickets) {
                (*events)[reservation.event_id].ticket_count += reservation.ticket_count;
                reservations_map.erase(reservation_id);
            }
        }
    }

    // Zwraca wskaźnik na rezerwację o danym identyfikatorze.
    Reservation &get_reservation(reservation_id_t reservation_id) {
        return reservations_map[reservation_id];
    }

    // Sprawdza, czy rezerwacja o danym identyfikatorze istnieje.
    bool check_if_reservation_exists(reservation_id_t reservation_id) {
        return reservations_map.count(reservation_id) > 0;
    }

    // Sprawdza, czy podane cookie jest poprawne.
    bool check_if_cookie_is_correct(reservation_id_t reservation_id, const std::string &cookie) {
        return reservations_map[reservation_id].cookie == cookie;
    }

    // Generuje odpowiednią liczbę biletów.
    std::vector<std::string> generate_tickets(ticket_count_t ticket_count) {
        std::vector<std::string> tickets;
        tickets.reserve(ticket_count);
        for (int i = 0; i < ticket_count; i++) {
            tickets.push_back(ticket_generator.generate_ticket());
        }
        return tickets;
    }

    // Oznacza daną rezerwację jako odebraną, ustawia flagę claimed_tickets na true i
    // generuje bilety.
    void claim_tickets(reservation_id_t reservation_id) {
        Reservation &reservation = reservations_map[reservation_id];
        reservation.claimed_tickets = true;
        reservation.tickets = generate_tickets(reservation.ticket_count);
    }

    // Sprawdza, czy serwer już co najmniej raz wysłał bilety (czy klient już co najmniej
    // raz je odebrał).
    bool client_already_claimed_tickets(reservation_id_t reservation_id) {
        return reservations_map[reservation_id].claimed_tickets;
    }
};

// Wstawia do buforu dane wydarzenie zgodnie ze specyfikacją komunikatu EVENTS, odpowiednio
// zwiększa wartość zmiennej send_length.
void insert_event_to_buffer(Event &event, event_id_t event_id, char *buffer, size_t &send_length) {
    insert_to_buffer(event_id, EVENT_ID_SIZE, buffer, send_length);
    insert_to_buffer(event.ticket_count, TICKET_COUNT_SIZE, buffer, send_length);
    insert_to_buffer(event.description_length, DESCRIPTION_LENGTH_SIZE, buffer, send_length);
    insert_to_buffer(event.description, event.description_length, buffer, send_length);
}

// Wypełnia bufor komunikatem EVENTS, odpowiednio zwiększa wartość zmiennej send_length.
void answer_get_events(std::vector<Event> &events, char *buffer, size_t &send_length,
                                                           std::string &message_name) {
    message_name = EVENTS_MESSAGE_NAME;
    insert_to_buffer(EVENTS, MESSAGE_ID_SIZE, buffer, send_length);
    for (size_t event_id = 0; event_id < events.size(); event_id++) {
        size_t next_event_size = events[event_id].size;
        if (send_length + next_event_size > BUFFER_SIZE) {
            return;
        }
        insert_event_to_buffer(events[event_id], event_id, buffer, send_length);
    }
}

// Sprawdza, czy wydarzenie o danym event_id istnieje.
bool check_if_event_exists(std::vector<Event> &events, event_id_t event_id) {
    return events.size() > event_id;
}

// Sprawdza, czy prośba z komunikatu GET_RESERVATION może zostać zrealizowana.
bool check_if_reservation_request_is_valid(std::vector<Event> &events, event_id_t event_id,
                                                               ticket_count_t ticket_count) {
    if (!check_if_event_exists(events, event_id) ||
        ticket_count == 0 ||
        ticket_count > events[event_id].ticket_count ||
        ticket_count > MAX_TICKETS_TO_SEND) {
        return false;
    }
    return true;
}

// Wypełnia bufor komunikatem RESERVATION, odpowiednio zwiększa wartość zmiennej send_length.
void insert_reservation_to_buffer(Reservation &reservation, reservation_id_t reservation_id, char *buffer,
                                                                                      size_t &send_length) {
    insert_to_buffer(RESERVATION, MESSAGE_ID_SIZE, buffer, send_length);
    insert_to_buffer(reservation_id, RESERVATION_ID_SIZE, buffer, send_length);
    insert_to_buffer(reservation.event_id, EVENT_ID_SIZE, buffer, send_length);
    insert_to_buffer(reservation.ticket_count, TICKET_COUNT_SIZE, buffer, send_length);
    insert_to_buffer(reservation.cookie, COOKIE_SIZE, buffer, send_length);
    insert_to_buffer(reservation.expiration_time, EXPIRATION_TIME_SIZE, buffer, send_length);
}

// Wypełnia bufor komunikatem BAD_REQUEST, odpowiednio zwiększa wartość zmiennej send_length.
void insert_bad_request_to_buffer(event_or_reservation_id_t event_or_reservation_id, char *buffer,
                                                                              size_t &send_length) {
    insert_to_buffer(BAD_REQUEST, MESSAGE_ID_SIZE, buffer, send_length);
    insert_to_buffer(event_or_reservation_id, EVENT_OR_RESERVATION_ID_SIZE, buffer, send_length);
}

// Sprawdza, czy prośba GET_RESERVATION może zostać zrealizowana, wypełnia bufor komunikatem
// RESERVATION lub BAD_REQUEST, odpowiednio zwiększa wartość zmiennej send_length.
void answer_get_reservation(std::vector<Event> &events, ReservationsHandler &reservations_handler,
                               expiration_time_t current_time, char *buffer, size_t parsed_length,
                                                   size_t &send_length, std::string &message_name) {
    event_id_t event_id;
    ticket_count_t ticket_count;
    get_from_buffer(event_id, EVENT_ID_SIZE, buffer, parsed_length);
    get_from_buffer(ticket_count, TICKET_COUNT_SIZE, buffer, parsed_length);

    if (check_if_reservation_request_is_valid(events, event_id, ticket_count)) {
        message_name = RESERVATION_MESSAGE_NAME;
        reservation_id_t reservation_id =
                reservations_handler.add_reservation(event_id, ticket_count, current_time);
        Reservation &reservation = reservations_handler.get_reservation(reservation_id);
        insert_reservation_to_buffer(reservation, reservation_id, buffer, send_length);
    } else {
        message_name = BAD_REQUEST_MESSAGE_NAME;
        insert_bad_request_to_buffer(event_id, buffer, send_length);
    }
}

// Sprawdza, czy prośba z komunikatu GET_TICKETS może zostać zrealizowana.
bool check_if_tickets_request_is_valid(ReservationsHandler &reservations_handler,
                      reservation_id_t reservation_id, const std::string &cookie) {
    if (!reservations_handler.check_if_reservation_exists(reservation_id) ||
        !reservations_handler.check_if_cookie_is_correct(reservation_id, cookie)) {
        return false;
    }
    return true;
}

// Wypełnia bufor komunikatem TICKETS, odpowiednio zwiększa wartość zmiennej send_length.
void insert_tickets_to_buffer(Reservation &reservation, reservation_id_t reservation_id, char *buffer,
                                                                                  size_t &send_length) {
    insert_to_buffer(TICKETS, MESSAGE_ID_SIZE, buffer, send_length);
    insert_to_buffer(reservation_id, RESERVATION_ID_SIZE, buffer, send_length);
    insert_to_buffer(reservation.ticket_count, TICKET_COUNT_SIZE, buffer, send_length);
    for (int i = 0; i < reservation.ticket_count; i++) {
        insert_to_buffer(reservation.tickets[i], TICKET_SIZE, buffer, send_length);
    }
}

// Sprawdza, czy prośba GET_TICKETS może zostać zrealizowana, wypełnia bufor komunikatem
// TICKETS lub BAD_REQUEST, odpowiednio zwiększa wartość zmiennej send_length.
void answer_get_tickets(ReservationsHandler &reservations_handler, char *buffer, size_t parsed_length,
                                                       size_t &send_length, std::string &message_name) {
    reservation_id_t reservation_id;
    std::string cookie;
    get_from_buffer(reservation_id, RESERVATION_ID_SIZE, buffer, parsed_length);
    get_from_buffer(cookie, COOKIE_SIZE, buffer, parsed_length);

    if (check_if_tickets_request_is_valid(reservations_handler, reservation_id, cookie)) {
        message_name = TICKETS_MESSAGE_NAME;
        if (!reservations_handler.client_already_claimed_tickets(reservation_id)) {
            reservations_handler.claim_tickets(reservation_id);
        }
        Reservation &reservation = reservations_handler.get_reservation(reservation_id);
        insert_tickets_to_buffer(reservation, reservation_id, buffer, send_length);
    } else {
        message_name = BAD_REQUEST_MESSAGE_NAME;
        insert_bad_request_to_buffer(reservation_id, buffer, send_length);
    }
}

// Sprawdza, czy serwer powinien odpowiedzieć na komunikat odebrany od klienta. Wczytuje nazwę
// komunikatu do zmiennej message_id i odpowiednio zwiększa wartość zmiennej parsed_length.
bool check_if_server_should_answer(size_t read_length, message_id_t &message_id, char *buffer,
                                                                         size_t &parsed_length) {
    if (read_length == 0) {
        if (debug) {
            fprintf(stderr, "Received message of incorrect length. "
                            "Server ignored the message.\n");
        }
        return false;
    }

    parsed_length = 0;
    get_from_buffer(message_id, MESSAGE_ID_SIZE, buffer, parsed_length);

    if (!(message_id == GET_EVENTS || message_id == GET_RESERVATION || message_id == GET_TICKETS)) {
        if (debug) {
            fprintf(stderr, "Received message with incorrect message_id. "
                            "Server ignored the message.\n");
        }
        return false;
    }

    if ((message_id == GET_EVENTS && read_length == GET_EVENTS_MESSAGE_LENGTH) ||
        (message_id == GET_RESERVATION && read_length == GET_RESERVATION_MESSAGE_LENGTH) ||
        (message_id == GET_TICKETS && read_length == GET_TICKETS_MESSAGE_LENGTH)) {
        return true;
    }

    if (debug) {
        fprintf(stderr, "Received message of incorrect length. "
                        "Server ignored the message.\n");
    }
    return false;
}

// Przetwarza komunikat od klienta i w zależności od nazwy otrzymanego komunikatu wypełnia bufor
// komunikatem EVENTS, RESERVATION, TICKETS lub BAD_REQUEST. Odpowiednio zwiększa wartość zmiennej
// send_length.
void process_request_and_prepare_answer(std::vector<Event> &events, ReservationsHandler &reservations_handler,
                                        expiration_time_t current_time, message_id_t message_id, char *buffer,
                                        size_t &parsed_length, size_t &send_length, std::string &message_name) {
    send_length = 0;
    switch (message_id) {
        case GET_EVENTS:
            answer_get_events(events, buffer, send_length, message_name);
            break;
        case GET_RESERVATION:
            answer_get_reservation(events, reservations_handler, current_time, buffer, parsed_length,
                                                                           send_length, message_name);
            break;
        case GET_TICKETS:
            answer_get_tickets(reservations_handler, buffer, parsed_length, send_length, message_name);
            break;
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    char *path;
    port_t port;
    timeout_t timeout;
    parse_parameters(argc, argv, path, port, timeout);

    std::vector<Event> events;
    load_events(path, events);
    ReservationsHandler reservations_handler(&events, timeout);

    memset(shared_buffer, 0, sizeof(shared_buffer));

    int socket_fd = bind_socket(port);
    if (debug) {
        fprintf(stderr, "Listening on port %u\n", port);
    }

    struct sockaddr_in client_address{};
    size_t read_length;
    size_t send_length;
    size_t parsed_length;
    message_id_t message_id;
    std::string message_name;
    do {
        read_length = read_message(socket_fd, &client_address,
                                   shared_buffer, sizeof(shared_buffer));
        expiration_time_t current_time = time(nullptr);
        reservations_handler.remove_expired_reservations(current_time);
        if (check_if_server_should_answer(read_length, message_id, shared_buffer, parsed_length)) {
            process_request_and_prepare_answer(events, reservations_handler, current_time, message_id,
                                               shared_buffer, parsed_length, send_length, message_name);
            send_message(socket_fd, &client_address, shared_buffer, send_length,
                         message_name);
        }
    } while (true);

    CHECK_ERRNO(close(socket_fd));

    return 0;
}