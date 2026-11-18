// =====================================================================
//  EduPulse AI - Exam Content Transformer  (C++17, works on Windows + macOS)
//
//  What it does:
//    1. You load a .txt study file (optional) and type a topic.
//    2. The program sends them to a FREE local AI (Ollama) over HTTP.
//    3. The AI replies in JSON.  We show it in 4 tabs:
//         Exam Summary | Explanation | Question Bank | Take Quiz
//
//  Libraries:  FLTK (windows/buttons)  httplib.h (HTTP)  json.hpp (JSON)
// =====================================================================

// NOTE: httplib must come BEFORE the FLTK headers (Windows socket headers).
#include "httplib.h"
#include "json.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Native_File_Chooser.H>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------
//  Data
// ---------------------------------------------------------------------
struct Question {
    std::string q;
    std::vector<std::string> opts;   // up to 4 options
    int ans = 0;                     // index of the correct option (0..3)
    std::string expl;                // why it is correct
};

struct Result {                      // what the background thread hands back
    bool ok = false;
    std::string err;
    json data;
};

static std::string g_source;         // text loaded from the .txt file
static std::vector<Question> g_quiz; // generated questions
static int  g_idx = 0, g_score = 0, g_answered_count = 0;
static bool g_answered = false;

// ---------------------------------------------------------------------
//  Widgets (global so callbacks can reach them)
// ---------------------------------------------------------------------
static Fl_Input      *in_topic, *in_model;
static Fl_Int_Input  *in_count;
static Fl_Choice     *ch_lang;
static Fl_Box        *path_box, *status_box, *q_label, *feedback, *score_box;
static Fl_Button     *btn_gen, *btn_submit, *btn_next;
static Fl_Round_Button *opt[4];
static Fl_Text_Buffer *buf_sum, *buf_exp, *buf_qb;
static Fl_Group      *quiz_group;

// ---------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------
// FLTK treats '&' and '@' in labels as special, so we double them.
static std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) { o += c; if (c == '&' || c == '@') o += c; }
    return o;
}

static void set_status(const std::string &s) {
    status_box->copy_label(esc(s).c_str());
    status_box->redraw();
}

static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back()))  s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

static std::string as_text(const json &j) {
    if (j.is_string()) return j.get<std::string>();
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

static std::string get_str(const json &d, const char *key) {
    return d.contains(key) ? as_text(d[key]) : std::string();
}

static std::string buffer_text(Fl_Text_Buffer *b) {
    char *t = b->text();
    std::string s = t ? t : "";
    free(t);
    return s;
}

// If the file is big, send only ~6000 characters around the topic.
static std::string pick_excerpt(const std::string &text, const std::string &topic) {
    const size_t MAXLEN = 6000;
    if (text.size() <= MAXLEN) return text;
    auto lower = [](std::string s) {
        for (auto &c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    size_t pos = lower(text).find(lower(topic));
    size_t start = 0;
    if (pos != std::string::npos && pos > 500) start = pos - 500;
    return text.substr(start, MAXLEN);
}

static int parse_answer(const json &a, size_t nopts) {
    int r = 0;
    if (a.is_number_integer()) r = a.get<int>();
    else if (a.is_string()) {
        std::string s = trim(a.get<std::string>());
        if (!s.empty()) {
            char c = (char)toupper((unsigned char)s[0]);
            if (c >= 'A' && c <= 'D') r = c - 'A';
            else if (c >= '0' && c <= '9') r = c - '0';
        }
    }
    if (r < 0 || r >= (int)nopts) r = 0;
    return r;
}

// ---------------------------------------------------------------------
//  Quiz tab logic
// ---------------------------------------------------------------------
static void update_score_box() {
    std::string s = "Score: " + std::to_string(g_score) + " / " +
                    std::to_string(g_answered_count);
    score_box->copy_label(s.c_str());
}

static void show_question() {
    g_answered = false;
    feedback->copy_label("");
    for (int i = 0; i < 4; i++) { opt[i]->value(0); opt[i]->hide(); }

    if (g_quiz.empty()) {
        q_label->copy_label("No quiz yet.\n\nType a topic and click Generate.");
        btn_submit->deactivate();
        btn_next->deactivate();
        quiz_group->redraw();
        return;
    }
    if (g_idx >= (int)g_quiz.size()) {            // finished
        std::string s = "Quiz finished!\n\nYour score: " + std::to_string(g_score) +
                        " / " + std::to_string((int)g_quiz.size());
        q_label->copy_label(s.c_str());
        btn_submit->deactivate();
        btn_next->label("Restart quiz");
        btn_next->activate();
        quiz_group->redraw();
        return;
    }
    const Question &q = g_quiz[g_idx];
    std::string head = "Question " + std::to_string(g_idx + 1) + " of " +
                       std::to_string(g_quiz.size()) + "\n\n" + q.q;
    q_label->copy_label(esc(head).c_str());
    for (size_t i = 0; i < q.opts.size() && i < 4; i++) {
        std::string t = std::string(1, "ABCD"[i]) + ")  " + q.opts[i];
        opt[i]->copy_label(esc(t).c_str());
        opt[i]->show();
    }
    btn_submit->activate();
    btn_next->label(g_idx + 1 == (int)g_quiz.size() ? "Finish" : "Next question");
    btn_next->activate();
    quiz_group->redraw();
}

static void restart_quiz() {
    g_idx = 0; g_score = 0; g_answered_count = 0;
    update_score_box();
    show_question();
}

static void submit_cb(Fl_Widget *, void *) {
    if (g_quiz.empty() || g_idx >= (int)g_quiz.size() || g_answered) return;
    int sel = -1;
    for (int i = 0; i < 4; i++)
        if (opt[i]->visible() && opt[i]->value()) sel = i;
    if (sel < 0) { feedback->copy_label("Please choose an option first."); return; }

    g_answered = true;
    g_answered_count++;
    const Question &q = g_quiz[g_idx];
    std::string msg;
    if (sel == q.ans) { g_score++; msg = "Correct!"; }
    else msg = std::string("Wrong. Correct answer: ") + "ABCD"[q.ans] + ")  " + q.opts[q.ans];
    if (!q.expl.empty()) msg += "\n\n" + q.expl;
    feedback->copy_label(esc(msg).c_str());
    update_score_box();
    quiz_group->redraw();
}

static void next_cb(Fl_Widget *, void *) {
    if (g_quiz.empty()) return;
    if (g_idx >= (int)g_quiz.size()) { restart_quiz(); return; }
    g_idx++;
    show_question();
}

// ---------------------------------------------------------------------
//  Fill the tabs from the AI's JSON
// ---------------------------------------------------------------------
static std::vector<std::string> get_list(const json &d, const char *key) {
    std::vector<std::string> v;
    if (d.contains(key) && d[key].is_array())
        for (auto &x : d[key]) v.push_back(as_text(x));
    return v;
}

static void fill_tabs(const json &d) {
    // --- Tab 1: Exam Summary ---
    std::string s = get_str(d, "summary") + "\n\nKey Points:\n";
    for (auto &p : get_list(d, "key_points")) s += "\xE2\x80\xA2 " + p + "\n";   // bullet
    s += "\nFormulas / Terms to Remember:\n";
    auto terms = get_list(d, "terms");
    if (terms.empty()) s += " None mentioned.\n";
    for (auto &t : terms) s += "- " + t + "\n";
    s += "\nCommon Exam Mistakes:\n";
    for (auto &m : get_list(d, "mistakes")) s += "- " + m + "\n";
    buf_sum->text(s.c_str());

    // --- Tab 2: Explanation ---
    buf_exp->text(get_str(d, "explanation").c_str());

    // --- Questions ---
    g_quiz.clear();
    if (d.contains("questions") && d["questions"].is_array()) {
        for (auto &jq : d["questions"]) {
            if (!jq.is_object()) continue;
            Question q;
            q.q = get_str(jq, "q");
            if (q.q.empty()) q.q = get_str(jq, "question");
            if (jq.contains("options") && jq["options"].is_array())
                for (auto &o : jq["options"]) q.opts.push_back(as_text(o));
            if (q.q.empty() || q.opts.size() < 2) continue;
            if (q.opts.size() > 4) q.opts.resize(4);
            q.ans  = parse_answer(jq.contains("answer") ? jq["answer"] : json(0), q.opts.size());
            q.expl = get_str(jq, "explanation");
            g_quiz.push_back(q);
        }
    }

    // --- Tab 3: Question Bank (printable list with answers) ---
    std::string qb;
    int n = 1;
    for (auto &q : g_quiz) {
        qb += "Q" + std::to_string(n++) + ". " + q.q + "\n";
        for (size_t i = 0; i < q.opts.size(); i++)
            qb += std::string("    ") + "ABCD"[i] + ") " + q.opts[i] + "\n";
        qb += std::string("Answer: ") + "ABCD"[q.ans] + "\n";
        if (!q.expl.empty()) qb += "Why: " + q.expl + "\n";
        qb += "\n";
    }
    if (qb.empty()) qb = "The AI did not return any questions. Click Generate again.";
    buf_qb->text(qb.c_str());

    // --- Tab 4: interactive quiz ---
    restart_quiz();
}

// ---------------------------------------------------------------------
//  Talking to the AI (runs in a background thread so the window stays alive)
// ---------------------------------------------------------------------
static void on_done(void *p) {                       // runs on the GUI thread
    std::unique_ptr<Result> r((Result *)p);
    btn_gen->activate();
    if (!r->ok) { set_status(r->err); return; }
    fill_tabs(r->data);
    set_status("Done! Use 'Save output' to keep it.");
}

static void worker(std::string model, std::string prompt) {
    Result *r = new Result();
    try {
        httplib::Client cli("127.0.0.1", 11434);     // Ollama's default address
        cli.set_connection_timeout(5);
        cli.set_read_timeout(600);                   // small laptops can be slow
        json body = {{"model", model},
                     {"prompt", prompt},
                     {"stream", false},
                     {"format", "json"},
                     {"options", {{"temperature", 0.3}}}};
        auto res = cli.Post("/api/generate",
                            body.dump(-1, ' ', false, json::error_handler_t::replace),
                            "application/json");
        if (!res) {
            r->err = "Cannot reach Ollama. Open the Ollama app (or run 'ollama serve') and try again.";
        } else if (res->status != 200) {
            r->err = "Ollama error " + std::to_string(res->status) +
                     ". Is the model installed? Run:  ollama pull " + model;
        } else {
            json outer = json::parse(res->body);
            std::string text = outer.value("response", "");
            size_t a = text.find('{'), b = text.rfind('}');
            if (a == std::string::npos || b == std::string::npos || b < a)
                throw std::runtime_error("no JSON found");
            r->data = json::parse(text.substr(a, b - a + 1));
            r->ok = true;
        }
    } catch (const std::exception &e) {
        r->ok = false;
        r->err = std::string("The AI's answer was not valid (") + e.what() +
                 "). Click Generate again.";
    }
    Fl::awake(on_done, r);                           // hand result to the GUI thread
}

static std::string build_prompt(const std::string &topic, const std::string &lang,
                                int n, const std::string &excerpt) {
    std::string langDesc = "simple English";
    if (lang.rfind("Hinglish", 0) == 0)
        langDesc = "Hinglish (Hindi written in Roman/English letters, mixed with English technical words)";
    else if (lang.rfind("Hindi", 0) == 0)
        langDesc = "Hindi (Devanagari script)";

    std::string p;
    p += "You are a friendly exam-preparation tutor for first-year B.Tech students in India.\n";
    p += "Topic: " + topic + "\n";
    p += "Write ALL text values in: " + langDesc + ".\n";
    if (!excerpt.empty())
        p += "Use ONLY the study material below.\n\n--- STUDY MATERIAL ---\n" + excerpt +
             "\n--- END ---\n\n";
    else
        p += "No study material was given, so use your own knowledge.\n\n";
    p += "Return ONLY valid JSON, exactly in this shape:\n";
    p += "{\"summary\":\"3-4 sentence summary\",";
    p += "\"key_points\":[\"point\",\"point\"],";
    p += "\"terms\":[\"term or formula: meaning\"],";
    p += "\"mistakes\":[\"common exam mistake\"],";
    p += "\"explanation\":\"a simple step-by-step explanation with an everyday example\",";
    p += "\"questions\":[{\"q\":\"question text\",\"options\":[\"opt1\",\"opt2\",\"opt3\",\"opt4\"],";
    p += "\"answer\":0,\"explanation\":\"why this option is correct\"}]}\n";
    p += "Rules: give exactly " + std::to_string(n) + " multiple-choice questions; ";
    p += "each has exactly 4 options; \"answer\" is the 0-based index (0,1,2 or 3) of the correct option; ";
    p += "put the correct option in different positions across questions; ";
    p += "5 key_points; if there are no formulas, use an empty list for terms.\n";
    return p;
}

// ---------------------------------------------------------------------
//  Button callbacks
// ---------------------------------------------------------------------
static void load_cb(Fl_Widget *, void *) {
    Fl_Native_File_Chooser fc;
    fc.title("Choose a .txt study file");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Text files\t*.txt");
    if (fc.show() != 0) return;                      // cancelled
    std::ifstream f(fc.filename(), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    g_source = ss.str();
    path_box->copy_label(esc(fc.filename()).c_str());
    set_status("Loaded " + std::to_string(g_source.size()) + " characters. Now type a topic and click Generate.");
}

static void generate_cb(Fl_Widget *, void *) {
    std::string topic = trim(in_topic->value());
    if (topic.empty()) { set_status("Please type a topic first (e.g. 'Quick sort')."); return; }
    int n = atoi(in_count->value());
    if (n < 1) n = 5;
    if (n > 15) n = 15;
    std::string model = trim(in_model->value());
    if (model.empty()) model = "llama3.2";
    std::string lang = ch_lang->text() ? ch_lang->text() : "English";

    std::string prompt = build_prompt(topic, lang, n, pick_excerpt(g_source, topic));
    btn_gen->deactivate();
    set_status("Generating... the first run can take 1-2 minutes. Please wait.");
    std::thread(worker, model, prompt).detach();
}

static void save_cb(Fl_Widget *, void *) {
    Fl_Native_File_Chooser fc;
    fc.title("Save output");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("Text files\t*.txt");
    fc.preset_file("edupulse_output.txt");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    if (fc.show() != 0) return;
    std::string path = fc.filename();
    if (path.size() < 4 || path.substr(path.size() - 4) != ".txt") path += ".txt";

    std::ofstream out(path, std::ios::binary);
    if (!out) { set_status("Could not write the file."); return; }
    out << "===== EXAM SUMMARY =====\n"   << buffer_text(buf_sum) << "\n\n";
    out << "===== EXPLANATION =====\n"    << buffer_text(buf_exp) << "\n\n";
    out << "===== QUESTION BANK =====\n"  << buffer_text(buf_qb)  << "\n";
    set_status("Saved to " + path);
}

// ---------------------------------------------------------------------
//  Build a read-only text area inside the current tab
// ---------------------------------------------------------------------
static Fl_Text_Buffer *make_text_tab(Fl_Tabs *tabs, const char *label) {
    Fl_Group *g = new Fl_Group(tabs->x(), tabs->y() + 25, tabs->w(), tabs->h() - 25, label);
    Fl_Text_Display *d = new Fl_Text_Display(g->x() + 5, g->y() + 5, g->w() - 10, g->h() - 10);
    Fl_Text_Buffer *b = new Fl_Text_Buffer();
    d->buffer(b);
    d->textsize(15);
    d->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    g->end();
    return b;
}

// ---------------------------------------------------------------------
//  main : build the window
// ---------------------------------------------------------------------
int main(int argc, char **argv) {
    Fl::scheme("gtk+");          // nicer look
    Fl::lock();                  // needed so the worker thread can call Fl::awake()

    Fl_Double_Window *win = new Fl_Double_Window(900, 700, "EduPulse AI - Exam Content Transformer");

    // ---- Row 1 : Load button, file path, Save button ----
    Fl_Button *btn_load = new Fl_Button(10, 10, 130, 32, "Load TXT");
    btn_load->callback(load_cb);
    path_box = new Fl_Box(150, 10, 540, 32, "No file loaded (optional)");
    path_box->box(FL_DOWN_BOX);
    path_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Button *btn_save = new Fl_Button(710, 10, 180, 32, "Save output");
    btn_save->callback(save_cb);

    // ---- Row 2 : Topic, Language, Generate ----
    in_topic = new Fl_Input(70, 52, 400, 30, "Topic:");
    ch_lang = new Fl_Choice(560, 52, 140, 30, "Language:");
    ch_lang->add("English|Hinglish (Roman script)|Hindi (Devanagari)");
    ch_lang->value(1);
    btn_gen = new Fl_Button(710, 52, 180, 30, "Generate");
    btn_gen->color(fl_rgb_color(30, 160, 60));
    btn_gen->labelcolor(FL_WHITE);
    btn_gen->labelfont(FL_HELVETICA_BOLD);
    btn_gen->callback(generate_cb);

    // ---- Row 3 : AI model, number of questions, hint ----
    in_model = new Fl_Input(70, 92, 260, 30, "AI model:");
    in_model->value("llama3.2");
    in_count = new Fl_Int_Input(430, 92, 60, 30, "Questions:");
    in_count->value("5");
    Fl_Box *hint = new Fl_Box(500, 92, 390, 30, "Free local AI (Ollama) must be running.");
    hint->labelsize(12);
    hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    // ---- Tabs ----
    Fl_Tabs *tabs = new Fl_Tabs(10, 135, 880, 520);
    buf_sum = make_text_tab(tabs, "Exam Summary");
    buf_exp = make_text_tab(tabs, "Explanation");
    buf_qb  = make_text_tab(tabs, "Question Bank");

    // Quiz tab (hand-built)
    quiz_group = new Fl_Group(10, 160, 880, 495, "Take Quiz");
    {
        q_label = new Fl_Box(25, 170, 850, 110, "");
        q_label->box(FL_FLAT_BOX);
        q_label->labelsize(17);
        q_label->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        for (int i = 0; i < 4; i++) {
            opt[i] = new Fl_Round_Button(35, 290 + i * 42, 830, 36, "");
            opt[i]->type(FL_RADIO_BUTTON);
            opt[i]->labelsize(15);
        }
        btn_submit = new Fl_Button(35, 465, 140, 34, "Submit answer");
        btn_submit->callback(submit_cb);
        btn_next = new Fl_Button(185, 465, 160, 34, "Next question");
        btn_next->callback(next_cb);
        score_box = new Fl_Box(720, 465, 150, 34, "Score: 0 / 0");
        score_box->labelfont(FL_HELVETICA_BOLD);
        feedback = new Fl_Box(35, 510, 830, 140, "");
        feedback->labelsize(15);
        feedback->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    }
    quiz_group->end();
    tabs->end();

    // ---- Status bar ----
    status_box = new Fl_Box(10, 665, 880, 28, "Ready. Type a topic and click Generate.");
    status_box->box(FL_DOWN_BOX);
    status_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);

    win->end();
    show_question();             // shows the "No quiz yet" message
    win->show(argc, argv);
    return Fl::run();
}
